// Reading what was copied without fetching anything: colours, links, code, and a light highlighter for code previews.
#include "shelf.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace sys::shelfkind {

namespace {

constexpr size_t kLinkMax = 2048; // longer is a text that happens to start with http
constexpr size_t kTitleMax = 90;
constexpr size_t kCodeScanMax = 8000; // enough of a long text to tell code from prose
constexpr float kCodeDensity = 0.06f; // brackets and operators per character, prose stays well under
constexpr int kIdLike = 8; // a path segment this long with digits in it is an id, not a title

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool StartsI(std::wstring_view s, std::wstring_view p) { return s.size() >= p.size() && _wcsnicmp(s.data(), p.data(), p.size()) == 0; }

int Hex(wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1; }

uint32_t Pack(double r, double g, double b) {
    auto q = [](double v) { return (uint32_t)std::lround((std::clamp)(v, 0.0, 255.0)); };
    return q(r) << 16 | q(g) << 8 | q(b);
}

void HslToRgb(double h, double s, double l, double& r, double& g, double& b) {
    h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 360.0;
    auto f = [&](double n) {
        double k = std::fmod(n + h * 12, 12.0), a = s * (std::min)(l, 1 - l);
        return l - a * (std::max)(-1.0, (std::min)({k - 3, 9 - k, 1.0}));
    };
    r = f(0) * 255, g = f(8) * 255, b = f(4) * 255;
}

bool Args(const std::wstring& inner, double (&v)[4], bool (&pct)[4], int& n) { // "255, 0, 0" or "255 0 0 / 50%"
    n = 0;
    const wchar_t* p = inner.c_str();
    while (*p && n < 4) {
        while (*p == L' ' || *p == L',' || *p == L'/' || *p == L'\t') ++p;
        if (!*p) break;
        wchar_t* end = nullptr;
        double d = wcstod(p, &end);
        if (end == p) return false;
        p = end;
        pct[n] = *p == L'%';
        if (pct[n]) ++p;
        while (iswalpha(*p)) ++p; // deg
        v[n++] = d;
    }
    while (*p == L' ') ++p;
    return !*p && n >= 3;
}

bool HasWord(std::wstring_view s, std::wstring_view w) {
    for (size_t at = s.find(w); at != std::wstring_view::npos; at = s.find(w, at + 1)) {
        bool l = at == 0 || !iswalnum(s[at - 1]), r = at + w.size() >= s.size() || !iswalnum(s[at + w.size()]);
        if (l && r) return true;
    }
    return false;
}

constexpr std::wstring_view kKeywords[] = { // across the usual languages, lowercase
    L"if", L"else", L"elif", L"for", L"while", L"do", L"switch", L"case", L"break", L"continue", L"return", L"function", L"fn", L"func", L"def",
    L"class", L"struct", L"enum", L"interface", L"impl", L"trait", L"public", L"private", L"protected", L"static", L"const", L"let", L"var",
    L"val", L"mut", L"new", L"delete", L"import", L"from", L"export", L"package", L"using", L"namespace", L"template", L"typename", L"void",
    L"int", L"long", L"short", L"float", L"double", L"bool", L"char", L"auto", L"true", L"false", L"null", L"nullptr", L"none", L"self",
    L"this", L"super", L"try", L"catch", L"finally", L"throw", L"throws", L"raise", L"except", L"async", L"await", L"yield", L"lambda",
    L"in", L"of", L"is", L"as", L"not", L"and", L"or", L"extends", L"implements", L"override", L"virtual", L"final", L"sizeof", L"typeof",
    L"instanceof", L"select", L"where", L"join", L"group", L"by", L"order", L"insert", L"update", L"into", L"values", L"create", L"table",
    L"drop", L"alter", L"set", L"with", L"go", L"defer", L"chan", L"type", L"match", L"loop", L"pub", L"mod", L"use", L"crate", L"unsafe",
    L"extern", L"pass", L"echo", L"then", L"fi", L"done", L"local", L"readonly", L"end", L"begin", L"module", L"require"};

constexpr std::wstring_view kStrong[] = { // words prose almost never uses: what tells code apart
    L"function", L"return", L"const", L"let", L"var", L"def", L"class", L"struct", L"import", L"public", L"private", L"static", L"void", L"int",
    L"bool", L"null", L"nullptr", L"true", L"false", L"else", L"elif", L"async", L"await", L"lambda", L"namespace", L"template", L"typename",
    L"sizeof", L"typeof", L"instanceof", L"fn", L"func", L"impl", L"pub", L"mut", L"enum", L"println", L"printf", L"console", L"self", L"this"};

constexpr std::wstring_view kStarts[] = { // a line opening with one of these, lowercase as written, is a statement
    L"def ", L"class ", L"return", L"import ", L"function", L"const ", L"let ", L"var ", L"public ", L"private ", L"static ", L"fn ", L"func ",
    L"if (", L"for (", L"while (", L"elif ", L"else", L"package ", L"using ", L"namespace ", L"struct ", L"enum ", L"async ", L"await ", L"try:",
    L"except", L"print(", L"echo ", L"export ", L"from "};

constexpr std::wstring_view kCodeHints[] = {L"=>", L"->", L"::", L"#include", L"();", L"){", L") {", L"</", L"/>", L"#!/", L"!=", L"==", L"&&", L"||", L"+="};

bool Keyword(std::wstring_view w) {
    if (w.size() < 2 || w.size() > 12) return false;
    wchar_t low[16];
    for (size_t i = 0; i < w.size(); ++i) low[i] = (wchar_t)towlower(w[i]);
    std::wstring_view l(low, w.size());
    return std::find(std::begin(kKeywords), std::end(kKeywords), l) != std::end(kKeywords);
}

std::wstring Decode(std::wstring_view s) { // %xx as utf-8, + as a space
    std::string bytes;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() && Hex(s[i + 1]) >= 0 && Hex(s[i + 2]) >= 0) {
            bytes += (char)(Hex(s[i + 1]) * 16 + Hex(s[i + 2]));
            i += 2;
        } else if (s[i] == L'+') {
            bytes += ' ';
        } else if (s[i] < 0x80) {
            bytes += (char)s[i];
        } else { // already wide: keep it as utf-8
            char buf[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, &s[i], 1, buf, sizeof(buf), nullptr, nullptr);
            bytes.append(buf, n > 0 ? n : 0);
        }
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), n);
    return out;
}

bool IdLike(std::wstring_view seg) { // 8f3a2c91d0, 12345: not words
    int digits = 0;
    for (wchar_t c : seg) digits += iswdigit(c) ? 1 : 0;
    return digits == (int)seg.size() || ((int)seg.size() >= kIdLike && digits * 3 >= (int)seg.size() && seg.find(L'-') == std::wstring_view::npos);
}

std::wstring Words(std::wstring_view seg) { // how-to-bake_bread.html -> How to bake bread
    std::wstring w = Decode(seg);
    size_t dot = w.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0 && w.size() - dot <= 5) w.resize(dot);
    for (wchar_t& c : w)
        if (c == L'-' || c == L'_' || c == L'+') c = L' ';
    std::wstring out;
    for (wchar_t c : w)
        if (!(c == L' ' && (out.empty() || out.back() == L' '))) out += c;
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (!out.empty()) out[0] = (wchar_t)towupper(out[0]);
    return out.size() > kTitleMax ? out.substr(0, kTitleMax) : out;
}

struct Rule {
    std::wstring_view key, type;
};
constexpr Rule kExtTypes[] = {{L"pdf", L"PDF document"}, {L"png", L"Image"}, {L"jpg", L"Image"}, {L"jpeg", L"Image"}, {L"gif", L"Image"}, {L"webp", L"Image"},
                              {L"svg", L"Image"}, {L"mp4", L"Video"}, {L"webm", L"Video"}, {L"mov", L"Video"}, {L"mp3", L"Audio"}, {L"wav", L"Audio"},
                              {L"flac", L"Audio"}, {L"zip", L"Archive"}, {L"7z", L"Archive"}, {L"rar", L"Archive"}, {L"gz", L"Archive"}, {L"exe", L"Download"},
                              {L"msi", L"Download"}, {L"docx", L"Document"}, {L"doc", L"Document"}, {L"xlsx", L"Spreadsheet"}, {L"csv", L"Spreadsheet"},
                              {L"pptx", L"Slides"}, {L"txt", L"Text file"}, {L"json", L"Data"}, {L"xml", L"Data"}};
constexpr Rule kSegTypes[] = {{L"watch", L"Video"}, {L"video", L"Video"}, {L"videos", L"Video"}, {L"embed", L"Video"}, {L"shorts", L"Video"}, {L"search", L"Search results"},
                              {L"issues", L"Issue"}, {L"issue", L"Issue"}, {L"pull", L"Code review"}, {L"pulls", L"Code review"}, {L"merge_requests", L"Code review"},
                              {L"blob", L"Code"}, {L"tree", L"Code"}, {L"commit", L"Code"}, {L"commits", L"Code"}, {L"wiki", L"Article"}, {L"blog", L"Article"},
                              {L"post", L"Article"}, {L"posts", L"Article"}, {L"article", L"Article"}, {L"articles", L"Article"}, {L"news", L"Article"},
                              {L"docs", L"Documentation"}, {L"documentation", L"Documentation"}, {L"reference", L"Documentation"}, {L"manual", L"Documentation"},
                              {L"guide", L"Documentation"}, {L"product", L"Product"}, {L"products", L"Product"}, {L"item", L"Product"}, {L"dp", L"Product"},
                              {L"questions", L"Discussion"}, {L"forum", L"Discussion"}, {L"thread", L"Discussion"}, {L"threads", L"Discussion"},
                              {L"comments", L"Discussion"}, {L"discussion", L"Discussion"}, {L"maps", L"Map"}, {L"place", L"Map"}, {L"status", L"Post"},
                              {L"playlist", L"Music"}, {L"album", L"Music"}, {L"track", L"Music"}, {L"artist", L"Music"}, {L"login", L"Sign-in page"},
                              {L"signin", L"Sign-in page"}, {L"signup", L"Sign-in page"}, {L"cart", L"Checkout"}, {L"checkout", L"Checkout"},
                              {L"user", L"Profile"}, {L"users", L"Profile"}, {L"profile", L"Profile"}, {L"people", L"Profile"}, {L"releases", L"Download"},
                              {L"download", L"Download"}, {L"downloads", L"Download"}, {L"event", L"Event"}, {L"events", L"Event"}};
constexpr Rule kHostTypes[] = {{L"docs.", L"Documentation"}, {L"maps.", L"Map"}, {L"mail.", L"Mail"}, {L"shop.", L"Store"}, {L"store.", L"Store"},
                               {L"news.", L"News"}, {L"music.", L"Music"}, {L"drive.", L"Files"}, {L"calendar.", L"Calendar"}, {L"blog.", L"Article"},
                               {L"forum.", L"Discussion"}, {L"wiki.", L"Article"}, {L"developer.", L"Documentation"}};

}

bool ParseColor(const std::wstring& raw, uint32_t& rgb) {
    std::wstring s = Lower(Trim(raw));
    if (s.size() > 64) return false;
    if (!s.empty() && s[0] == L'#') {
        size_t n = s.size() - 1;
        if (n != 3 && n != 4 && n != 6 && n != 8) return false;
        for (size_t i = 1; i < s.size(); ++i)
            if (Hex(s[i]) < 0) return false;
        if (n <= 4) rgb = (uint32_t)(Hex(s[1]) * 17) << 16 | (uint32_t)(Hex(s[2]) * 17) << 8 | (uint32_t)(Hex(s[3]) * 17); // #abc is #aabbcc
        else rgb = (uint32_t)wcstoul(s.substr(1, 6).c_str(), nullptr, 16); // alpha, if any, is left off
        return true;
    }
    size_t open = s.find(L'('), close = s.rfind(L')');
    if (open == std::wstring::npos || close != s.size() - 1) return false;
    std::wstring fn = Trim(s.substr(0, open)), inner = s.substr(open + 1, close - open - 1);
    double v[4];
    bool pct[4];
    int n = 0;
    if (!Args(inner, v, pct, n)) return false;
    if (fn == L"rgb" || fn == L"rgba") {
        for (int i = 0; i < 3; ++i)
            if (pct[i]) v[i] *= 2.55;
        rgb = Pack(v[0], v[1], v[2]);
        return true;
    }
    if (fn == L"hsl" || fn == L"hsla") {
        double r, g, b;
        HslToRgb(v[0], (std::clamp)(v[1] / 100, 0.0, 1.0), (std::clamp)(v[2] / 100, 0.0, 1.0), r, g, b);
        rgb = Pack(r, g, b);
        return true;
    }
    return false;
}

std::wstring ColorText(uint32_t rgb, int how) {
    int r = rgb >> 16 & 0xFF, g = rgb >> 8 & 0xFF, b = rgb & 0xFF;
    wchar_t buf[48];
    if (how == 1) {
        swprintf_s(buf, L"rgb(%d, %d, %d)", r, g, b);
    } else if (how == 2) {
        double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0, hi = (std::max)({rf, gf, bf}), lo = (std::min)({rf, gf, bf}), d = hi - lo, l = (hi + lo) / 2;
        double s = d == 0 ? 0 : d / (1 - std::fabs(2 * l - 1)), h = 0;
        if (d > 0) h = hi == rf ? std::fmod((gf - bf) / d + 6, 6.0) : hi == gf ? (bf - rf) / d + 2 : (rf - gf) / d + 4;
        swprintf_s(buf, L"hsl(%d, %d%%, %d%%)", (int)std::lround(h * 60) % 360, (int)std::lround(s * 100), (int)std::lround(l * 100));
    } else {
        swprintf_s(buf, L"#%06X", rgb & 0xFFFFFF);
    }
    return buf;
}

bool IsLink(const std::wstring& raw) {
    std::wstring s = Trim(raw);
    if (s.size() < 8 || s.size() > kLinkMax || s.find_first_of(L" \t\r\n\"<>") != std::wstring::npos) return false;
    if (StartsI(s, L"http://") || StartsI(s, L"https://")) return s.find(L'.') != std::wstring::npos || s.find(L"localhost") != std::wstring::npos;
    return StartsI(s, L"www.") && s.find(L'.', 4) != std::wstring::npos;
}

Link ReadLink(const std::wstring& raw) {
    std::wstring url = Trim(raw);
    size_t at = url.find(L"://");
    at = at == std::wstring::npos ? 0 : at + 3;
    size_t hostEnd = url.find_first_of(L"/?#", at);
    if (hostEnd == std::wstring::npos) hostEnd = url.size();
    std::wstring host = Lower(url.substr(at, hostEnd - at));
    if (size_t cred = host.rfind(L'@'); cred != std::wstring::npos) host = host.substr(cred + 1); // never shows a user:password
    if (size_t port = host.find(L':'); port != std::wstring::npos) host.resize(port);
    Link l;
    l.site = StartsI(host, L"www.") ? host.substr(4) : StartsI(host, L"m.") ? host.substr(2) : host;
    size_t qAt = url.find_first_of(L"?#", hostEnd);
    std::wstring_view path(url.c_str() + hostEnd, (qAt == std::wstring::npos ? url.size() : qAt) - hostEnd);
    std::wstring query = qAt != std::wstring::npos && url[qAt] == L'?' ? url.substr(qAt + 1, url.find(L'#', qAt) - qAt - 1) : L"";
    std::vector<std::wstring_view> segs;
    for (size_t i = 0; i < path.size();) {
        size_t j = path.find(L'/', i);
        if (j == std::wstring_view::npos) j = path.size();
        if (j > i) segs.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    for (const Rule& r : kHostTypes)
        if (StartsI(host, r.key)) l.type = r.type;
    for (auto seg : segs) {
        std::wstring low = Lower(std::wstring(seg));
        for (const Rule& r : kSegTypes)
            if (low == r.key) l.type = r.type; // the deepest that says something wins
        if (!low.empty() && low[0] == L'@') l.type = L"Profile";
    }
    if (!segs.empty()) {
        std::wstring last = Lower(std::wstring(segs.back()));
        size_t dot = last.find_last_of(L'.');
        if (dot != std::wstring::npos)
            for (const Rule& r : kExtTypes)
                if (last.compare(dot + 1, std::wstring::npos, r.key) == 0) l.type = r.type;
    }
    std::wstring q;
    for (size_t i = 0; i < query.size();) { // a search's words make the best title
        size_t j = (std::min)(query.find(L'&', i), query.size());
        std::wstring_view kv(query.c_str() + i, j - i);
        if (StartsI(kv, L"q=") || StartsI(kv, L"query=") || StartsI(kv, L"search=")) q = Decode(kv.substr(kv.find(L'=') + 1));
        i = j + 1;
    }
    if (!q.empty() && l.type.empty()) l.type = L"Search results";
    for (size_t i = segs.size(); i-- > 0 && l.title.empty();)
        if (!IdLike(segs[i])) l.title = Words(segs[i]);
    if (!q.empty() && (l.type == L"Search results" || l.title.empty())) l.title = L"“" + q.substr(0, kTitleMax) + L"”";
    if (l.type.empty()) l.type = segs.empty() && query.empty() ? L"Home page" : L"Web page";
    if (l.title.empty()) l.title = l.site;
    return l;
}

std::wstring CleanLink(const std::wstring& raw) {
    static constexpr std::wstring_view kTrack[] = {L"fbclid", L"gclid", L"gclsrc", L"dclid", L"gbraid", L"wbraid", L"msclkid", L"mc_cid", L"mc_eid", L"igshid",
                                                   L"igsh", L"yclid", L"twclid", L"ttclid", L"li_fat_id", L"_hsenc", L"_hsmi", L"mkt_tok", L"oly_anon_id",
                                                   L"oly_enc_id", L"vero_id", L"wickedid", L"s_cid", L"ref_src", L"ref_url", L"si", L"spm", L"scid", L"srsltid",
                                                   L"_ga", L"_gl", L"trk", L"trkinfo"};
    std::wstring url = Trim(raw);
    size_t q = url.find(L'?');
    if (q == std::wstring::npos) return url;
    size_t hash = url.find(L'#', q);
    std::wstring frag = hash == std::wstring::npos ? L"" : url.substr(hash), query = url.substr(q + 1, (hash == std::wstring::npos ? url.size() : hash) - q - 1);
    std::wstring kept;
    for (size_t i = 0; i <= query.size();) {
        size_t j = (std::min)(query.find(L'&', i), query.size());
        std::wstring kv = query.substr(i, j - i), key = Lower(kv.substr(0, kv.find(L'=')));
        bool track = StartsI(key, L"utm_") || std::find(std::begin(kTrack), std::end(kTrack), key) != std::end(kTrack);
        if (!kv.empty() && !track) kept += (kept.empty() ? L"" : L"&") + kv;
        i = j + 1;
    }
    return url.substr(0, q) + (kept.empty() ? L"" : L"?" + kept) + frag;
}

bool LooksLikeCode(const std::wstring& text) {
    std::wstring_view s(text.c_str(), (std::min)(text.size(), kCodeScanMax));
    int lines = 0, sig = 0, dense = 0, words = 0;
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(L'\n', i);
        if (j == std::wstring_view::npos) j = s.size();
        std::wstring_view line = s.substr(i, j - i);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.remove_suffix(1);
        size_t lead = line.find_first_not_of(L" \t");
        i = j + 1;
        if (lead == std::wstring_view::npos) continue;
        ++lines;
        std::wstring_view body = line.substr(lead);
        wchar_t end = line.back();
        bool hint = std::any_of(std::begin(kCodeHints), std::end(kCodeHints), [&](std::wstring_view h) { return body.find(h) != std::wstring_view::npos; }) ||
                    std::any_of(std::begin(kStarts), std::end(kStarts), [&](std::wstring_view h) { return body.substr(0, h.size()) == h; }); // case kept: prose starts with a capital
        if (end == L';' || end == L'{' || end == L'}' || body[0] == L'}' || StartsI(body, L"//") || StartsI(body, L"#") || StartsI(body, L"<") || hint) ++sig;
        else if (lead > 0 && (end == L')' || end == L':' || end == L',')) ++sig; // an indented call or block opener
    }
    for (wchar_t c : s) dense += wcschr(L"{}[]();=<>", c) && c ? 1 : 0;
    for (auto kw : kStrong) words += HasWord(s, kw) ? 1 : 0;
    float density = s.empty() ? 0 : dense / (float)s.size();
    if (lines >= 2) return sig * 2 >= lines && (words >= 1 || density > kCodeDensity);
    return words >= 2 && density > kCodeDensity && (s.back() == L';' || s.back() == L'}' || s.back() == L')');
}

ShelfKind Classify(const std::wstring& text, uint32_t& rgb) {
    if (ParseColor(text, rgb)) return ShelfKind::Color;
    if (IsLink(text)) return ShelfKind::Link;
    if (LooksLikeCode(text)) return ShelfKind::Code;
    return ShelfKind::Text;
}

bool IsImageName(const std::wstring& name) {
    static constexpr std::wstring_view kExt[] = {L"png", L"jpg", L"jpeg", L"jfif", L"gif", L"bmp", L"webp", L"tif", L"tiff", L"heic", L"heif", L"avif", L"ico"};
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || name.find_first_of(L"\\/", dot) != std::wstring::npos) return false;
    std::wstring ext = Lower(name.substr(dot + 1));
    return std::find(std::begin(kExt), std::end(kExt), ext) != std::end(kExt);
}

std::vector<Span> Highlight(const std::wstring& code) {
    std::vector<Span> out;
    const size_t n = code.size();
    auto add = [&](size_t a, size_t b, Role r) {
        if (b > a) out.push_back({(uint32_t)a, (uint32_t)(b - a), r});
    };
    bool lineStart = true; // only blanks so far on this line
    for (size_t i = 0; i < n;) {
        wchar_t c = code[i];
        if (c == L'\n' || c == L' ' || c == L'\t' || c == L'\r') {
            lineStart = lineStart || c == L'\n';
            ++i;
            continue;
        }
        bool fresh = lineStart;
        lineStart = false;
        if (c == L'/' && i + 1 < n && code[i + 1] == L'/') { // a line comment
            size_t e = code.find(L'\n', i);
            e = e == std::wstring::npos ? n : e;
            add(i, e, kComment);
            i = e;
        } else if (c == L'/' && i + 1 < n && code[i + 1] == L'*') {
            size_t e = code.find(L"*/", i + 2);
            e = e == std::wstring::npos ? n : e + 2;
            add(i, e, kComment);
            i = e;
        } else if (c == L'#' && (fresh || (i > 0 && iswspace(code[i - 1])))) { // a directive, or a shell or python comment
            size_t e = i + 1;
            if (e < n && iswalpha(code[e])) {
                while (e < n && iswalnum(code[e])) ++e;
                add(i, e, kKeyword);
            } else {
                e = code.find(L'\n', i);
                e = e == std::wstring::npos ? n : e;
                add(i, e, kComment);
            }
            i = e;
        } else if (c == L'"' || c == L'\'' || c == L'`') {
            size_t e = i + 1;
            while (e < n && code[e] != c && (c == L'`' || code[e] != L'\n')) e += code[e] == L'\\' ? 2 : 1;
            e = (std::min)(n, e + 1);
            add(i, e, kString);
            i = e;
        } else if (iswdigit(c) || (c == L'.' && i + 1 < n && iswdigit(code[i + 1]))) {
            size_t e = i + 1;
            while (e < n && (iswalnum(code[e]) || code[e] == L'.' || code[e] == L'_')) ++e;
            add(i, e, kNumber);
            i = e;
        } else if (iswalpha(c) || c == L'_' || c == L'$' || c == L'@') {
            size_t e = i + 1;
            while (e < n && (iswalnum(code[e]) || code[e] == L'_' || code[e] == L'$')) ++e;
            if (Keyword(std::wstring_view(code.c_str() + i, e - i))) add(i, e, kKeyword);
            i = e;
        } else {
            if (wcschr(L"{}[]()<>=;:,.+-*/%!&|^~?", c) && c) add(i, i + 1, kPunct);
            ++i;
        }
    }
    return out;
}

}
