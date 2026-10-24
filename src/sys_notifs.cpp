#include "notifs.h"

#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <mutex>
#include <string_view>
#include <unordered_map>

#pragma comment(lib, "winsqlite3.lib")  // windows' own sqlite, ships in system32

namespace sys {
namespace {

constexpr int kBusyMs = 200;  // windows may be mid-write; wait this long, then give up for this read
constexpr size_t kMaxRows = 500;  // the db keeps ~100 toasts, this only bounds a bad caller
constexpr size_t kMaxPayload = 64 * 1024;  // toasts are a few kb; anything larger is cut, not parsed whole
constexpr size_t kTitleCap = 200;  // chars
constexpr size_t kBodyCap = 400;  // chars, the panel shows a few lines at most
constexpr size_t kNameCacheMax = 128;  // distinct apps; cleared wholesale past this
constexpr DWORD kNoValue = 0xFFFFFFFF;  // ToastEnabled missing
constexpr wchar_t kPushKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";  // where settings keeps the toast master switch
constexpr wchar_t kAppsFolder[] = L"shell:AppsFolder\\";  // the virtual folder that knows every app by its aumid

std::wstring Wide(std::string_view s) {  // utf-8 -> utf-16; bad bytes become U+FFFD
    if (s.empty() || s.size() > INT_MAX) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(std::wstring_view w) {
    if (w.empty() || w.size() > INT_MAX) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring DbPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};  // unset or too long: no notifications rather than a wrong path
    return std::wstring(buf, n) + L"\\Microsoft\\Windows\\Notifications\\wpndatabase.db";  // action center's own store, per user
}

std::string FileUri(const std::wstring& path) {  // sqlite uri: forward slashes, reserved chars %-escaped
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string u = "file:///";
    for (unsigned char c : Utf8(path)) {
        if (c == '\\') u += '/';  // uris use forward slashes even on windows
        else if (c == '%' || c == '?' || c == '#' || c == ' ' || c < 0x20 || c >= 0x80) {
            u += '%';
            u += kHex[c >> 4];
            u += kHex[c & 15];
        } else u += static_cast<char>(c);
    }
    return u + "?mode=ro";  // read-only: we must never write to windows' database
}

int64_t NowFiletime() {  // the db keeps times as filetimes, so expiry compares directly
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// ---- minimal toast xml ----

void PutUtf8(std::string& out, uint32_t cp) {  // encode one code point
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool NumEntity(std::string_view e, uint32_t& cp) {  // "#123" or "#x1F600"
    if (e.size() < 2 || e[0] != '#') return false;
    bool hex = e[1] == 'x' || e[1] == 'X';
    size_t i = hex ? 2 : 1;
    if (i >= e.size() || e.size() - i > 8) return false;  // overflow guard
    uint32_t v = 0;
    for (; i < e.size(); ++i) {
        char c = e[i];
        int d = c >= '0' && c <= '9' ? c - '0'
              : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
              : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;  // a stray character: not an entity, kept as text
        v = v * (hex ? 16 : 10) + static_cast<uint32_t>(d);
    }
    if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return false;  // not a scalar value
    cp = v;
    return true;
}

std::string Decode(std::string_view s) {  // xml entities -> utf-8; unknown ones kept verbatim
    std::string out;
    out.reserve(s.size());  // decoding only ever shrinks
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {  // no entity is that long: a bare ampersand
            out += '&';
            continue;
        }
        std::string_view e = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (NumEntity(e, cp)) PutUtf8(out, cp);
        else {
            out += '&';
            continue;
        }
        i = semi;  // the loop's ++i steps past the ';'
    }
    return out;
}

std::string_view Attr(std::string_view tag, std::string_view name) {  // raw value of name="..." inside one start tag
    for (size_t p = 0; (p = tag.find(name, p)) != std::string_view::npos; p += name.size()) {
        if (p == 0 || !isspace(static_cast<unsigned char>(tag[p - 1]))) continue;  // "xlaunch=" is not "launch="
        size_t q = p + name.size();
        while (q < tag.size() && isspace(static_cast<unsigned char>(tag[q]))) ++q;  // xml allows space around '='
        if (q >= tag.size() || tag[q] != '=') continue;  // the name appeared inside another value
        ++q;
        while (q < tag.size() && isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;  // either quote, as xml allows
        size_t end = tag.find(tag[q], q + 1);  // the matching quote
        if (end == std::string_view::npos) return {};  // unterminated: no value
        return tag.substr(q + 1, end - q - 1);
    }
    return {};
}

bool IsTag(std::string_view x, size_t p, std::string_view name) {  // "<name" followed by a delimiter, not "<names"
    if (x.compare(p, name.size(), name) != 0) return false;
    size_t q = p + name.size();
    return q < x.size() && (x[q] == '>' || x[q] == '/' || isspace(static_cast<unsigned char>(x[q])));
}

std::string_view StartTag(std::string_view x, size_t p) {  // from '<' to the closing '>'
    size_t e = x.find('>', p);
    return e == std::string_view::npos ? x.substr(p) : x.substr(p, e - p + 1);  // a '>' inside a quoted value would cut it short; toasts don't do that
}

std::string Trim(std::string s) {
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };  // xml whitespace; isspace would depend on the locale
    size_t a = 0, b = s.size();
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string_view Binding(std::string_view x) {  // the ToastGeneric binding, else the first one, else everything
    size_t first = std::string_view::npos;
    for (size_t p = 0; (p = x.find("<binding", p)) != std::string_view::npos; p += 8) {
        if (!IsTag(x, p, "<binding")) continue;
        if (first == std::string_view::npos) first = p;  // the fallback, if none is generic
        if (Attr(StartTag(x, p), "template").find("ToastGeneric") != std::string_view::npos) {  // the modern template; older ones are ToastText02 and kin
            first = p;
            break;
        }
    }
    if (first == std::string_view::npos) return x;  // no bindings at all: search the whole toast
    size_t end = x.find("</binding>", first);  // missing: run to the end
    return x.substr(first, end == std::string_view::npos ? std::string_view::npos : end - first);
}

std::vector<std::string> Texts(std::string_view b) {  // <text> contents in order, decoded and trimmed
    std::vector<std::string> out;
    for (size_t p = 0; (p = b.find("<text", p)) != std::string_view::npos; p += 5) {
        if (!IsTag(b, p, "<text")) continue;
        std::string_view tag = StartTag(b, p);
        if (tag.size() >= 2 && tag[tag.size() - 2] == '/') continue;  // <text/>
        size_t a = p + tag.size();
        size_t e = b.find("</text>", a);
        if (e == std::string_view::npos) break;  // truncated payload: stop at what's whole
        std::string_view raw = b.substr(a, e - a);
        std::string t;
        if (raw.substr(0, 9) == "<![CDATA[" && raw.size() >= 12 && raw.substr(raw.size() - 3) == "]]>")
            t = std::string(raw.substr(9, raw.size() - 12));  // cdata is literal, no entity decoding
        else t = Decode(raw);
        t = Trim(std::move(t));
        if (!t.empty()) out.push_back(std::move(t));  // empty lines are placeholders some apps send
        p = e;
    }
    return out;
}

std::wstring Cap(std::wstring s, size_t cap) {
    if (s.size() <= cap) return s;
    size_t n = cap;
    if (n > 0 && IS_HIGH_SURROGATE(s[n - 1])) --n;  // don't split a pair
    s.resize(n);
    s += L'…';
    return s;
}

// ---- image resolution ----

bool Exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string UrlDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        if (s[i] == '%' && i + 2 < s.size() && hv(s[i + 1]) >= 0 && hv(s[i + 2]) >= 0) {  // a malformed escape passes through as is
            out += static_cast<char>(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::wstring Family(const std::wstring& aumid) {  // "Pkg_hash!App" -> "Pkg_hash"; desktop ids have no '!'
    size_t bang = aumid.find(L'!');
    return bang == std::wstring::npos ? std::wstring() : aumid.substr(0, bang);
}

std::wstring PackageDir(const std::wstring& family) {  // install dir of the first package in the family
    if (family.empty()) return {};
    UINT32 count = 0, len = 0;
    if (GetPackagesByPackageFamily(family.c_str(), &count, nullptr, &len, nullptr) != ERROR_INSUFFICIENT_BUFFER || !count || !len) return {};
    std::vector<PWSTR> names(count);
    std::wstring buf(len, L'\0');
    if (GetPackagesByPackageFamily(family.c_str(), &count, names.data(), &len, buf.data()) != ERROR_SUCCESS || !count) return {};
    UINT32 plen = 0;
    if (GetPackagePathByFullName(names[0], &plen, nullptr) != ERROR_INSUFFICIENT_BUFFER || !plen) return {};
    std::wstring path(plen, L'\0');
    if (GetPackagePathByFullName(names[0], &plen, path.data()) != ERROR_SUCCESS) return {};
    path.resize(wcslen(path.c_str()));  // the length counted the terminator
    return path;
}

std::wstring Qualified(const std::wstring& p) {  // ms-appx names "Logo.png" but the file is often "Logo.scale-200.png"
    if (Exists(p)) return p;
    size_t dot = p.find_last_of(L'.');
    size_t slash = p.find_last_of(L'\\');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};  // no extension to qualify
    for (const wchar_t* q : {L".scale-200", L".scale-100", L".scale-150", L".targetsize-48", L".targetsize-32"}) {
        std::wstring c = p.substr(0, dot) + q + p.substr(dot);
        if (Exists(c)) return c;
    }
    return {};
}

std::wstring LocalPath(std::string_view rest) {  // "a/b%20c.png" -> L"a\\b c.png", no ".." escapes
    std::string s = UrlDecode(rest.substr(0, rest.find_first_of("?#")));  // query and fragment are not part of the file
    if (s.find("..") != std::string::npos) return {};  // checked after decoding, so %2e%2e can't slip past
    std::wstring w = Wide(s);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    while (!w.empty() && w.front() == L'\\') w.erase(w.begin());  // always relative to the package
    return w;
}

bool StartsI(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && _strnicmp(s.data(), p.data(), p.size()) == 0;
}

std::wstring ResolveImage(std::string_view src, const std::wstring& aumid) {  // only local files; web images are skipped
    if (src.empty()) return {};
    if (StartsI(src, "file:///")) {
        std::wstring p = Wide(UrlDecode(src.substr(8)));  // past "file:///", leaving "C:/..."
        std::replace(p.begin(), p.end(), L'/', L'\\');
        return Exists(p) ? p : std::wstring();
    }
    if (StartsI(src, "ms-appx:///")) {
        std::wstring dir = PackageDir(Family(aumid));
        std::wstring rel = LocalPath(src.substr(11));
        return dir.empty() || rel.empty() ? std::wstring() : Qualified(dir + L"\\" + rel);
    }
    if (StartsI(src, "ms-appdata:///local/")) {
        std::wstring fam = Family(aumid);
        std::wstring rel = LocalPath(src.substr(20));
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (fam.empty() || rel.empty() || n == 0 || n >= MAX_PATH) return {};
        return Qualified(std::wstring(buf, n) + L"\\Packages\\" + fam + L"\\LocalState\\" + rel);  // where an app's local folder really lives
    }
    if (src.size() > 2 && src[1] == ':' && (src[2] == '\\' || src[2] == '/')) {  // plain absolute path
        std::wstring p = Wide(src);
        return Exists(p) ? p : std::wstring();
    }
    return {};
}

std::string_view LogoSrc(std::string_view b) {  // <image placement="appLogoOverride" src=...>
    for (size_t p = 0; (p = b.find("<image", p)) != std::string_view::npos; p += 6) {
        if (!IsTag(b, p, "<image")) continue;
        std::string_view tag = StartTag(b, p);
        if (Attr(tag, "placement") == "appLogoOverride") return Attr(tag, "src");
    }
    return {};
}

std::string_view Payload(const void* data, int bytes, std::string& scratch) {  // handles utf-8 (with or without bom) and utf-16le
    if (!data || bytes <= 0) return {};
    const auto* p = static_cast<const unsigned char*>(data);
    size_t n = (std::min)(static_cast<size_t>(bytes), kMaxPayload);
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {  // never seen, but cheap to accept
        std::wstring_view w(reinterpret_cast<const wchar_t*>(p + 2), (n - 2) / 2);  // wchar_t is utf-16le on windows
        scratch = Utf8(w);
        return scratch;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }  // skip a utf-8 bom
    return std::string_view(reinterpret_cast<const char*>(p), n);
}

bool Parse(std::string_view xml, const std::wstring& aumid, Notif& n) {  // false = nothing worth showing
    size_t t = xml.find("<toast");
    while (t != std::string_view::npos && !IsTag(xml, t, "<toast")) t = xml.find("<toast", t + 6);
    if (t == std::string_view::npos) return false;  // not a toast payload
    n.launch = Wide(Decode(Attr(StartTag(xml, t), "launch")));  // what the app wants back when its toast is clicked
    std::string_view b = Binding(xml.substr(t));
    std::vector<std::string> texts = Texts(b);
    if (texts.empty()) return false;  // image-only or progress shells; nothing to read
    n.title = Cap(Wide(texts[0]), kTitleCap);  // the first text is the title by toast convention
    std::string body;
    for (size_t i = 1; i < texts.size() && body.size() < kBodyCap * 4; ++i) {  // *4: utf-8 bytes per char upper bound
        if (!body.empty()) body += '\n';
        body += texts[i];
    }
    n.body = Cap(Wide(body), kBodyCap);
    n.image = ResolveImage(Decode(LogoSrc(b)), aumid);
    return true;
}

// ---- db ----

class Db {  // raii read-only connection; closed when the read is over
public:
    Db() {
        std::wstring path = DbPath();
        if (path.empty()) return;  // db_ stays null; every query then returns nothing
        if (sqlite3_open_v2(FileUri(path).c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            OutputDebugStringW(L"[notifs] open failed\n");
            Close();  // sqlite hands back a handle even on failure; it must still be closed
            return;
        }
        sqlite3_busy_timeout(db_, kBusyMs);
    }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db() { Close(); }
    sqlite3* get() const { return db_; }

private:
    void Close() {
        if (db_) sqlite3_close(db_);  // every statement is finalized before this
        db_ = nullptr;
    }
    sqlite3* db_ = nullptr;
};

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (db && sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK) st_ = nullptr;  // e.g. a future schema without these columns
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    ~Stmt() { sqlite3_finalize(st_); }  // null safe
    sqlite3_stmt* get() const { return st_; }

private:
    sqlite3_stmt* st_ = nullptr;
};

constexpr char kQuery[] =
    "SELECT n.Id, h.PrimaryId, n.ArrivalTime, n.Payload "
    "FROM Notification n JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
    "WHERE n.Type = 'toast' AND (n.ExpiryTime IS NULL OR n.ExpiryTime = 0 OR n.ExpiryTime > ?1) "
    "ORDER BY n.ArrivalTime DESC, n.[Order] DESC LIMIT ?2";

std::wstring ColText(sqlite3_stmt* st, int col) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
    int n = sqlite3_column_bytes(st, col);  // after column_text, so it counts the utf-8 bytes
    return p && n > 0 ? Wide(std::string_view(p, static_cast<size_t>(n))) : std::wstring();
}

std::vector<Notif> Query(size_t max, bool& ok) {
    std::vector<Notif> out;
    ok = false;
    Db db;
    Stmt st(db.get(), kQuery);
    if (!st.get()) return out;  // no db, locked past the timeout, or a schema we don't know
    // rows past `max` may parse to nothing, so fetch a little extra and stop once full
    sqlite3_bind_int64(st.get(), 1, NowFiletime());
    sqlite3_bind_int64(st.get(), 2, static_cast<sqlite3_int64>((std::min)(max * 2, kMaxRows)));
    int rc = SQLITE_DONE;
    while (out.size() < max && (rc = sqlite3_step(st.get())) == SQLITE_ROW) {
        Notif n;
        n.id = sqlite3_column_int64(st.get(), 0);
        n.aumid = ColText(st.get(), 1);
        n.arrived = sqlite3_column_int64(st.get(), 2);
        std::string scratch;
        const void* blob = sqlite3_column_blob(st.get(), 3);  // works for TEXT and BLOB columns alike
        std::string_view xml = Payload(blob, sqlite3_column_bytes(st.get(), 3), scratch);
        if (xml.empty() || !Parse(xml, n.aumid, n)) continue;  // silent toasts and progress shells are skipped
        out.push_back(std::move(n));
    }
    ok = rc == SQLITE_DONE || rc == SQLITE_ROW; // read to the end or filled up; busy or broken is no answer, not "nothing new"
    return out;
}

// ---- names ----

std::wstring LastSegment(const std::wstring& aumid) {  // readable guess from the id itself
    if (_wcsicmp(aumid.c_str(), L"Windows.Defender.SecurityCenter") == 0) return L"Windows Security";  // not in AppsFolder
    if (_wcsnicmp(aumid.c_str(), L"Windows.SystemToast.", 20) == 0) return L"Windows";  // shell's own toasts
    std::wstring s = aumid;
    size_t bang = s.find(L'!');
    if (bang != std::wstring::npos) {  // packaged: "Microsoft.ScreenSketch_8wekyb3d8bbwe!App" -> "ScreenSketch"
        s.resize(bang);
        size_t us = s.find_last_of(L'_');
        if (us != std::wstring::npos) s.resize(us);
    } else {
        size_t slash = s.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {  // desktop path id: "...\powershell.exe" -> "powershell"
            s.erase(0, slash + 1);
            size_t dot = s.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) s.resize(dot);
            return s.empty() ? aumid : s;
        }
    }
    size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < s.size()) s.erase(0, dot + 1);  // "Company.Product" -> "Product"
    return s.empty() ? aumid : s;
}

std::wstring ShellName(const std::wstring& aumid) {  // needs com; initialise here if the caller's thread didn't
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring name;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName((kAppsFolder + aumid).c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
        PWSTR dn = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &dn)) && dn) {  // the name start shows, localized
            name = dn;
            CoTaskMemFree(dn);
        }
        item->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();  // S_FALSE also needs balancing; RPC_E_CHANGED_MODE does not
    return name;
}

bool LooksLikeUri(const std::wstring& s) {  // "scheme:rest", no spaces, not a drive letter or a file/shell launch
    size_t colon = s.find(L':');
    if (colon < 2 || colon == std::wstring::npos || colon + 1 >= s.size()) return false;
    if (s.find_first_of(L" \t\r\n") != std::wstring::npos) return false;  // launch args like "action=reply id=4" aren't uris
    if (!iswalpha(s[0])) return false;  // rfc 3986: a scheme starts with a letter
    for (size_t i = 1; i < colon; ++i)
        if (!iswalnum(s[i]) && s[i] != L'+' && s[i] != L'-' && s[i] != L'.') return false;
    std::wstring scheme = s.substr(0, colon);
    CharLowerBuffW(scheme.data(), static_cast<DWORD>(scheme.size()));
    return scheme != L"file" && scheme != L"shell";  // these could run local programs; windows' own click wouldn't via launch args
}

bool Shell(const std::wstring& target) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;  // no error dialogs from a taskbar; finish before we return
    sei.lpVerb = L"open";
    sei.lpFile = target.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

}  // namespace

std::vector<Notif> ReadNotifs(size_t max, bool* ok) noexcept {
    bool read = false;
    if (ok) *ok = false;
    try {
        if (max == 0) return {};
        std::vector<Notif> out = Query((std::min)(max, kMaxRows), read);
        if (ok) *ok = read;
        for (Notif& n : out) n.app = AppNameOf(n.aumid);  // after the db is closed, shell lookups can be slow
        return out;
    } catch (...) {
        OutputDebugStringW(L"[notifs] read failed\n");  // oom or similar; an empty list is a fine answer
        return {};
    }
}

int64_t NotifStamp() noexcept {
    try {
        std::wstring db = DbPath();
        if (db.empty()) return 0;
        uint64_t h = 1469598103934665603ull;  // fnv-1a over times and sizes of the db and its wal
        for (const std::wstring& p : {db, db + L"-wal"}) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) continue;  // wal is absent after a clean close
            const DWORD parts[] = {fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime, fa.nFileSizeLow, fa.nFileSizeHigh};
            for (DWORD v : parts) {
                h ^= v;
                h *= 1099511628211ull;
            }
        }
        return static_cast<int64_t>(h);  // changes whenever windows writes a toast; the caller re-reads only then
    } catch (...) {
        return 0;
    }
}

bool OpenNotifApp(const Notif& n) noexcept {
    try {
        AllowSetForegroundWindow(ASFW_ANY);  // the bar never activates, so let whatever we start take the foreground
        if (LooksLikeUri(n.launch) && Shell(n.launch)) return true;  // a deep link straight to what the toast was about
        if (n.aumid.empty()) return false;
        return Shell(kAppsFolder + n.aumid);
    } catch (...) {
        return false;
    }
}

std::wstring AppNameOf(const std::wstring& aumid) noexcept {
    try {
        if (aumid.empty()) return {};
        static std::mutex lock;  // ReadNotifs may run off the ui thread
        static std::unordered_map<std::wstring, std::wstring> cache;
        {
            std::lock_guard<std::mutex> g(lock);
            auto it = cache.find(aumid);
            if (it != cache.end()) return it->second;
        }
        std::wstring name = ShellName(aumid);  // outside the lock, it can take a few ms
        if (name.empty()) name = LastSegment(aumid);  // not in the apps folder: a guess from the id
        std::lock_guard<std::mutex> g(lock);
        if (cache.size() >= kNameCacheMax) cache.clear();  // bounded, not lru: app churn is tiny
        cache.emplace(aumid, name);
        return name;
    } catch (...) {
        return aumid;  // the raw id beats no name at all
    }
}

// windows 11's real do-not-disturb has no public api: its state is an undocumented binary wnf blob
// (WNF_SHEL_QUIETHOURS_*) plus cloudstore data, and writing those wrong can wedge the quick settings toggle.
// instead this is the documented-in-practice master switch that settings > notifications writes: ToastEnabled = 0
// stops toast banners and sounds for every app. toasts still land in action center (so the panel keeps working),
// it doesn't show the moon glyph in windows' own ui, and it doesn't reflect dnd turned on from quick settings.
bool SetDoNotDisturb(bool on) noexcept {
    DWORD v = on ? 0 : 1;  // inverted: the value means "toasts enabled"
    return RegSetKeyValueW(HKEY_CURRENT_USER, kPushKey, L"ToastEnabled", REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
}

bool DoNotDisturb() noexcept {
    DWORD v = kNoValue, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kPushKey, L"ToastEnabled", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;  // missing = windows default, banners on
    return v == 0;
}

}  // namespace sys
