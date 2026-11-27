// A game's cover, fetched once and kept on disk. A steam game's comes straight from steam. Any other is looked for by name: in
// steam's public store first (no key, most pc games are on it), taken only when the title matches outright, then in igdb, which
// knows the games steam doesn't have. igdb is asked through astraeus' small relay, which holds the twitch keys so no copy of the
// app carries them; a pc with keys of its own (src/igdb_keys.h, or IGDB_CLIENT_ID and IGDB_CLIENT_SECRET) asks igdb directly.
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "games.h"
#include "shelf_impl.h"
#include "sys.h"

#if __has_include("igdb_keys.h")
#include "igdb_keys.h" // defines kIgdbId and kIgdbSecret; written by the release build, never committed
#else
constexpr const wchar_t* kIgdbId = L"";
constexpr const wchar_t* kIgdbSecret = L"";
#endif

namespace sys::games {
namespace {

constexpr size_t kSearchMax = 128 * 1024, kPictureMax = 4u << 20, kTokenMax = 4096;
constexpr DWORD kTimeoutMs = 8000;
constexpr int kIgdbLimit = 10;
constexpr const wchar_t* kSteamSearch = L"https://store.steampowered.com/api/storesearch/?l=english&cc=US&term=";
constexpr const wchar_t* kSteamShapes[] = {L"/library_600x900_2x.jpg", L"/library_600x900.jpg", L"/header.jpg"}; // tall first, like the launchers' own
constexpr const wchar_t* kSteamHosts[] = {L"https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/", L"https://cdn.cloudflare.steamstatic.com/steam/apps/"};
constexpr const wchar_t* kRelay = L"https://astraeus.astraeus-app.workers.dev/cover?name="; // the cover relay: igdb-relay/worker.js
constexpr const wchar_t* kIgdbPicture = L"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/";

std::wstring token; // twitch's app token for igdb: about two months good, asked again when igdb refuses it. worker thread only

std::wstring Encode(const std::wstring& s) { // a url's query: utf-8, anything but letters and digits escaped
    std::wstring out;
    for (unsigned char b : shelfio::Utf8(s)) {
        if (isalnum(b) || b == '-' || b == '_' || b == '.' || b == '~') {
            out += (wchar_t)b;
        } else {
            wchar_t hex[4];
            swprintf_s(hex, L"%%%02X", b);
            out += hex;
        }
    }
    return out;
}

std::wstring Plain(const std::wstring& s) { // for comparing titles: letters and digits, lowercase; "Alan Wake II™" and "alan wake ii" agree
    std::wstring out;
    for (wchar_t c : s)
        if (iswalnum(c)) out += (wchar_t)towlower(c);
    return out;
}

bool Get(const std::wstring& url, std::string& body, size_t max) { // one redirect followed: steam's pictures move between hosts
    std::atomic<bool> stop{false};
    std::atomic<void*> slot{nullptr};
    std::wstring moved;
    if (detail::HttpGet(url.c_str(), body, stop, slot, max, &moved) && !body.empty()) return true;
    return !moved.empty() && detail::HttpGet(moved.c_str(), body, stop, slot, max) && !body.empty();
}

int Post(const wchar_t* host, const std::wstring& path, const std::wstring& headers, const std::string& data, std::string& body) { // the http status, 0 when it never got there
    body.clear();
    int status = 0;
    HINTERNET s = WinHttpOpen(L"Astraeus", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (q && WinHttpSendRequest(q, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1L, (LPVOID)data.data(), (DWORD)data.size(), (DWORD)data.size(), 0) &&
        WinHttpReceiveResponse(q, nullptr)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        char buf[8192];
        DWORD got = 0;
        while (body.size() < kSearchMax && WinHttpReadData(q, buf, sizeof(buf), &got) && got) body.append(buf, got);
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return status;
}

std::vector<std::string> Objects(const std::string& json) { // each object directly inside the top array, as text
    std::vector<std::string> out;
    int depth = 0;
    bool str = false;
    size_t from = 0;
    for (size_t i = 0; i < json.size(); ++i) {
        char ch = json[i];
        if (str) {
            if (ch == '\\') ++i;
            else if (ch == '"') str = false;
            continue;
        }
        if (ch == '"') str = true;
        else if (ch == '[' || ch == '{') {
            if (ch == '{' && depth == 1) from = i;
            ++depth;
        } else if (ch == ']' || ch == '}') {
            --depth;
            if (ch == '}' && depth == 1) out.push_back(json.substr(from, i - from + 1));
        }
    }
    return out;
}

std::wstring Field(const std::string& obj, const char* key) { // a string or number value; \uXXXX dropped, \" and \\ undone: enough for titles and ids
    std::string k = std::string("\"") + key + "\":";
    size_t at = obj.find(k);
    if (at == std::string::npos) return {};
    at += k.size();
    std::string raw;
    if (at < obj.size() && obj[at] == '"') {
        for (++at; at < obj.size() && obj[at] != '"'; ++at) {
            if (obj[at] != '\\' || at + 1 >= obj.size()) raw += obj[at];
            else if (obj[++at] == 'u') at += 4;
            else raw += obj[at];
        }
    } else {
        while (at < obj.size() && isdigit((unsigned char)obj[at])) raw += obj[at++];
    }
    return shelfio::Wide(raw);
}

bool IsPicture(const std::string& b) { // a jpeg or a png, not an error page
    return b.size() > 8 && (((unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) || b.compare(1, 3, "PNG") == 0);
}

std::wstring Key(const wchar_t* name, const wchar_t* built) { // built in, or this pc's environment
    if (*built) return built;
    wchar_t v[256];
    DWORD n = GetEnvironmentVariableW(name, v, 256);
    return n && n < 256 ? std::wstring(v, n) : L"";
}

bool Token(const std::wstring& id, const std::wstring& secret) {
    std::string body;
    std::wstring path = L"/oauth2/token?grant_type=client_credentials&client_id=" + Encode(id) + L"&client_secret=" + Encode(secret);
    if (Post(L"id.twitch.tv", path, L"", "", body) != 200) return false;
    token = Field(body, "access_token");
    return !token.empty() && body.size() < kTokenMax;
}

int SteamSearch(const std::wstring& name, std::wstring& appid) { // only an outright match: "alan wake 2" mustn't come back as the first one
    std::string body;
    if (!Get(kSteamSearch + Encode(name), body, kSearchMax)) return kArtOffline;
    std::wstring want = Plain(name);
    for (const std::string& item : Objects(body.substr((std::min)(body.find("\"items\""), body.size()))))
        if (Plain(Field(item, "name")) == want && !(appid = Field(item, "id")).empty()) return kArtFound;
    return kArtNone;
}

int RelaySearch(const std::wstring& name, std::wstring& image) { // igdb through astraeus' own relay, which holds the keys: what everyone gets
    if (!*kRelay) return kArtNone;
    std::string body;
    if (!Get(kRelay + Encode(name), body, kSearchMax)) return kArtOffline;
    image = Field(body, "image_id");
    return image.empty() ? kArtNone : kArtFound;
}

int IgdbSearch(const std::wstring& name, std::wstring& image) { // the outright match if there is one, else igdb's best guess
    std::wstring id = Key(L"IGDB_CLIENT_ID", kIgdbId), secret = Key(L"IGDB_CLIENT_SECRET", kIgdbSecret);
    if (id.empty() || secret.empty()) return RelaySearch(name, image); // keys of its own on this pc ask igdb directly
    std::wstring quoted;
    for (wchar_t c : name)
        if (c != L'"' && c != L'\\') quoted += c;
    std::string query = "search \"" + shelfio::Utf8(quoted) + "\"; fields name,cover.image_id; where cover != null & version_parent = null; limit " + std::to_string(kIgdbLimit) + ";";
    std::string body;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (token.empty() && !Token(id, secret)) return Online() ? kArtNone : kArtOffline;
        int status = Post(L"api.igdb.com", L"/v4/games", L"Client-ID: " + id + L"\r\nAuthorization: Bearer " + token + L"\r\nContent-Type: text/plain\r\n", query, body);
        if (status == 200) break;
        if (status == 0) return kArtOffline;
        token.clear(); // refused: the token ran out, a fresh one is asked for once
        if (attempt) return kArtNone;
    }
    std::vector<std::string> games = Objects(body);
    std::wstring want = Plain(name);
    for (const std::string& g : games)
        if (Plain(Field(g, "name")) == want && !(image = Field(g, "image_id")).empty()) return kArtFound;
    if (!games.empty() && !(image = Field(games[0], "image_id")).empty()) return kArtFound;
    return kArtNone;
}

int Save(const std::wstring& file, const std::vector<std::wstring>& urls) {
    std::string pic;
    for (const auto& url : urls)
        if (!IsPicture(pic)) Get(url, pic, kPictureMax);
    if (!IsPicture(pic)) return Online() ? kArtNone : kArtOffline; // offline: asked again another time
    CreateDirectoryW(file.substr(0, file.find_last_of(L'\\')).c_str(), nullptr);
    return shelfio::WriteAtomic(file, pic) ? kArtFound : kArtNone;
}

std::vector<std::wstring> SteamUrls(const std::wstring& appid) {
    std::vector<std::wstring> out;
    for (const wchar_t* host : kSteamHosts)
        for (const wchar_t* shape : kSteamShapes) out.push_back(host + appid + shape);
    return out;
}

}

int FindArt(const std::wstring& id, const std::wstring& name, std::wstring& key, std::wstring& file) {
    key.clear(), file.clear();
    std::wstring appid;
    int steam = id.rfind(L"steam:", 0) == 0 ? (appid = id.substr(6), kArtFound) : SteamSearch(name, appid);
    if (steam == kArtFound) {
        key = appid, file = CoverFile(key);
        if (file.empty()) return kArtNone;
        if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return kArtFound;
        if (Save(file, SteamUrls(appid)) == kArtFound) return kArtFound;
    }
    std::wstring image;
    int igdb = IgdbSearch(name, image);
    if (igdb != kArtFound) return steam == kArtOffline ? kArtOffline : igdb;
    key = L"igdb-" + image, file = CoverFile(key);
    if (file.empty()) return kArtNone;
    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return kArtFound;
    return Save(file, {kIgdbPicture + image + L".jpg"});
}

std::wstring CoverFile(const std::wstring& key) {
    std::wstring dir = shelfio::Dir();
    return dir.empty() || key.empty() ? L"" : dir + L"\\gamecovers\\" + key + L".jpg";
}

}
