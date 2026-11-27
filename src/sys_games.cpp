// Games are found the way their launchers left them: steam's library folders and app manifests, epic's install manifests, gog's
// registry entries, the folders the xbox app and riot install into, and windows' own record of programs it took for games.
// A running program is a game when it lives under one of those install folders, so a windowed game counts as much as a fullscreen one.
#include "games.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>

#pragma comment(lib, "version.lib")

namespace sys::games {
namespace {

constexpr size_t kMaxFile = 4u << 20; // a manifest is a few kilobytes; anything huge isn't one
constexpr int kCoverDepth = 3; // steam keeps a game's art a folder or two down
constexpr const wchar_t* kSourceNames[kSourceCount] = {L"Steam", L"Epic Games", L"GOG", L"Ubisoft Connect", L"Microsoft Store", L"Riot Games", L"Windows", L"Recognised", L"Added by you"};
constexpr const wchar_t* kHelpers[] = {L"crash", L"unins", L"setup", L"redist", L"anticheat", L"easyanticheat", L"battleye", L"beservice", L"dxsetup", L"vc_redist", L"installer", L"updater", L"reporter", L"cefprocess", L"webhelper", L"steamwebhelper", L"overlay"}; // programs that ship beside a game and aren't it. a launcher isn't here: a game may be called one, and its own launcher just opens the session early
constexpr const wchar_t* kEngineFiles[] = {L"UnityPlayer.dll", L"GameAssembly.dll", L"steam_api64.dll", L"steam_api.dll", L"EOSSDK-Win64-Shipping.dll", L"Galaxy64.dll", L"Galaxy.dll"}; // beside a program: an engine's runtime or a store's game add-in, which only games ship
constexpr const wchar_t* kUnrealTail = L"-shipping.exe"; // how unreal names a finished game's program
constexpr const wchar_t* kJavaFiles[] = {L"javaw.exe", L"java.exe"};
constexpr const wchar_t* kGameWindows[] = {L"GLFW30", L"LWJGL"}; // the window a java game opens: minecraft and all that's built on it
constexpr const wchar_t* kNotGames[] = {L"\\windows\\", L"\\microsoft\\edge", L"\\google\\chrome", L"\\mozilla firefox\\"}; // never sniffed, whatever sits beside them
constexpr const wchar_t* kNotGameDirs[] = {L"client", L"gamesave"}; // riot's launcher, the xbox app's saves
constexpr const wchar_t* kSteamTools[] = {L"steamworks common", L"steam linux runtime", L"proton ", L"steamvr"}; // in the library, never played

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring ReadFile(const std::wstring& path) { // utf-8 text, whole; empty if missing or absurd
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.size() > kMaxFile ? std::wstring() : Wide(s);
}

std::wstring Quoted(const std::wstring& text, const wchar_t* key, size_t from = 0, size_t* next = nullptr) { // the quoted value after "key", in valve's files and json alike; \\ and \" undone
    std::wstring want = std::wstring(L"\"") + key + L"\"";
    size_t at = text.find(want, from);
    if (at == std::wstring::npos) return {};
    size_t q = text.find(L'"', at + want.size());
    if (q == std::wstring::npos) return {};
    std::wstring out;
    size_t i = q + 1;
    for (; i < text.size() && text[i] != L'"'; ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) ++i; // the next one as it is
        out += text[i];
    }
    if (next) *next = i;
    return out;
}

std::wstring Dir(std::wstring path) { // lowercase, backslashes, one at the end
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path.empty() ? path : Lower(path) + L"\\";
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring FindUnder(const std::wstring& dir, const wchar_t* file, int depth) { // the first file of that name in dir or below
    if (Exists(dir + L"\\" + file)) return dir + L"\\" + file;
    if (depth <= 0) return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        found = FindUnder(dir + L"\\" + fd.cFileName, file, depth - 1);
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::wstring SteamCover(const std::wstring& steam, const std::wstring& appid) { // the tall library picture steam already downloaded, the wide one failing that
    std::wstring cache = steam + L"\\appcache\\librarycache";
    for (const wchar_t* flat : {L"_library_600x900.jpg", L"_header.jpg"})
        if (Exists(cache + L"\\" + appid + flat)) return cache + L"\\" + appid + flat;
    if (!Exists(cache + L"\\" + appid)) return {};
    for (const wchar_t* name : {L"library_600x900.jpg", L"library_capsule.jpg", L"header.jpg", L"library_header.jpg"})
        if (std::wstring hit = FindUnder(cache + L"\\" + appid, name, kCoverDepth); !hit.empty()) return hit;
    return {};
}

std::wstring RegText(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS) return {};
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return std::wstring(buf, wcsnlen(buf, size / sizeof(wchar_t)));
}

template <class F> void EachSubkey(HKEY root, const wchar_t* key, REGSAM view, F&& f) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_ENUMERATE_SUB_KEYS | view, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        f(std::wstring(key) + L"\\" + name, std::wstring(name));
    }
    RegCloseKey(k);
}

void Steam(std::vector<Game>& out) {
    std::wstring steam = RegText(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) return;
    std::replace(steam.begin(), steam.end(), L'/', L'\\');
    std::vector<std::wstring> libs{steam};
    std::wstring folders = ReadFile(steam + L"\\steamapps\\libraryfolders.vdf");
    for (size_t at = 0;;) { // every "path" in it is a library
        size_t next = 0;
        std::wstring path = Quoted(folders, L"path", at, &next);
        if (path.empty()) break;
        at = next;
        if (std::none_of(libs.begin(), libs.end(), [&](const std::wstring& l) { return Dir(l) == Dir(path); })) libs.push_back(path);
    }
    for (const std::wstring& lib : libs) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((lib + L"\\steamapps\\appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring acf = ReadFile(lib + L"\\steamapps\\" + fd.cFileName);
            std::wstring appid = Quoted(acf, L"appid"), name = Quoted(acf, L"name"), dir = Quoted(acf, L"installdir");
            if (appid.empty() || name.empty() || dir.empty()) continue;
            std::wstring low = Lower(name);
            if (std::any_of(std::begin(kSteamTools), std::end(kSteamTools), [&](const wchar_t* t) { return low.find(t) != std::wstring::npos; })) continue;
            Game g;
            g.id = L"steam:" + appid, g.name = name, g.source = kSteam;
            g.dir = Dir(lib + L"\\steamapps\\common\\" + dir);
            g.cover = SteamCover(steam, appid);
            out.push_back(std::move(g));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void Epic(std::vector<Game>& out) {
    wchar_t data[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", data, MAX_PATH)) return;
    std::wstring dir = std::wstring(data) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.item").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring item = ReadFile(dir + L"\\" + fd.cFileName);
        std::wstring name = Quoted(item, L"DisplayName"), where = Quoted(item, L"InstallLocation"), app = Quoted(item, L"AppName");
        if (name.empty() || where.empty()) continue;
        Game g;
        g.id = L"epic:" + (app.empty() ? name : app), g.name = name, g.source = kEpic, g.dir = Dir(where);
        out.push_back(std::move(g));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void Gog(std::vector<Game>& out) {
    EachSubkey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", KEY_WOW64_32KEY, [&](const std::wstring& key, const std::wstring& id) {
        std::wstring name = RegText(HKEY_LOCAL_MACHINE, key.c_str(), L"gameName", KEY_WOW64_32KEY), where = RegText(HKEY_LOCAL_MACHINE, key.c_str(), L"path", KEY_WOW64_32KEY);
        if (name.empty() || where.empty()) return;
        Game g;
        g.id = L"gog:" + id, g.name = name, g.source = kGog, g.dir = Dir(where);
        out.push_back(std::move(g));
    });
}

void Ubisoft(std::vector<Game>& out) { // games ubisoft's own launcher installed: only where they are, so the folder names them
    EachSubkey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Ubisoft\\Launcher\\Installs", KEY_WOW64_32KEY, [&](const std::wstring& key, const std::wstring& id) {
        std::wstring where = RegText(HKEY_LOCAL_MACHINE, key.c_str(), L"InstallDir", KEY_WOW64_32KEY);
        std::wstring dir = Dir(where);
        if (dir.empty()) return;
        std::replace(where.begin(), where.end(), L'/', L'\\');
        while (!where.empty() && where.back() == L'\\') where.pop_back();
        size_t slash = where.find_last_of(L'\\');
        Game g;
        g.id = L"ubi:" + id, g.name = slash == std::wstring::npos ? where : where.substr(slash + 1), g.source = kUbisoft, g.dir = dir;
        out.push_back(std::move(g));
    });
}

void Folders(std::vector<Game>& out, const wchar_t* parent, int source, const wchar_t* tag) { // a launcher that gives each game a folder of its own under one parent, on any drive
    wchar_t drives[256];
    DWORD len = GetLogicalDriveStringsW(256, drives);
    for (const wchar_t* d = drives; d < drives + len && *d; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
        std::wstring root = std::wstring(d) + parent;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring low = Lower(fd.cFileName);
            if (std::any_of(std::begin(kNotGameDirs), std::end(kNotGameDirs), [&](const wchar_t* n) { return low.find(n) != std::wstring::npos; })) continue; // the launcher's own folders sit among its games
            Game g;
            g.id = std::wstring(tag) + L":" + Lower(fd.cFileName), g.name = fd.cFileName, g.source = source, g.dir = Dir(root + L"\\" + fd.cFileName);
            out.push_back(std::move(g));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void Windows(std::vector<Game>& out) { // programs windows itself took for games, for game mode and the game bar
    EachSubkey(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, [&](const std::wstring& key, const std::wstring& id) {
        std::wstring exe = RegText(HKEY_CURRENT_USER, key.c_str(), L"MatchedExeFullPath");
        if (exe.empty() || !Exists(exe)) return;
        Game g = Marked(exe);
        g.id = L"win:" + id, g.source = kWindows;
        out.push_back(std::move(g));
    });
}

}

const wchar_t* SourceName(int source) { return kSourceNames[(std::clamp)(source, 0, kSourceCount - 1)]; }

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring ExeOf(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[1024];
    DWORD len = 1024;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != 0;
    CloseHandle(h);
    return ok ? std::wstring(path, len) : std::wstring();
}

std::wstring ProductName(const std::wstring& exe) {
    std::wstring name;
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (size) {
        std::vector<BYTE> info(size);
        struct Lang { WORD lang, page; }* langs = nullptr;
        UINT n = 0;
        if (GetFileVersionInfoW(exe.c_str(), 0, size, info.data()) && VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &n) && n >= sizeof(Lang)) {
            for (const wchar_t* field : {L"ProductName", L"FileDescription"}) {
                wchar_t query[80];
                swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].page, field);
                wchar_t* text = nullptr;
                UINT chars = 0;
                if (name.empty() && VerQueryValueW(info.data(), query, reinterpret_cast<void**>(&text), &chars) && chars > 1) name.assign(text, wcsnlen(text, chars));
            }
        }
    }
    if (!name.empty()) return name;
    size_t slash = exe.find_last_of(L"\\/"), dot = exe.rfind(L'.');
    size_t from = slash == std::wstring::npos ? 0 : slash + 1;
    return exe.substr(from, dot != std::wstring::npos && dot > from ? dot - from : std::wstring::npos);
}

Game Marked(const std::wstring& exe) {
    Game g;
    g.exe = Lower(exe);
    std::replace(g.exe.begin(), g.exe.end(), L'/', L'\\');
    g.id = L"exe:" + g.exe, g.name = ProductName(exe), g.source = kMarked;
    return g;
}

std::vector<Game> Scan() {
    std::vector<Game> out;
    Steam(out);
    Epic(out);
    Gog(out);
    Ubisoft(out);
    Folders(out, L"XboxGames", kXbox, L"xbox");
    Folders(out, L"Riot Games", kRiot, L"riot");
    size_t launchers = out.size();
    Windows(out);
    out.erase(std::remove_if(out.begin() + launchers, out.end(), [&](const Game& g) { // windows lists played launcher games too, under the file name: the launcher's entry has the real title
        return std::any_of(out.begin(), out.begin() + launchers, [&](const Game& l) { return !l.dir.empty() && g.exe.compare(0, l.dir.size(), l.dir) == 0; });
    }), out.end());
    out.erase(std::remove_if(out.begin(), out.end(), [](const Game& g) { return g.dir.empty() ? g.exe.empty() : !Exists(g.dir); }), out.end()); // listed but since uninstalled
    return out;
}

int Match(const std::vector<Game>& games, const std::wstring& exeLower) {
    if (exeLower.empty()) return -1;
    for (size_t i = 0; i < games.size(); ++i)
        if (!games[i].exe.empty() && games[i].exe == exeLower) return (int)i; // named outright, by a launcher or by hand: whatever it's called
    size_t slash = exeLower.find_last_of(L'\\');
    std::wstring file = slash == std::wstring::npos ? exeLower : exeLower.substr(slash + 1);
    for (const wchar_t* helper : kHelpers)
        if (file.find(helper) != std::wstring::npos) return -1;
    int best = -1;
    size_t depth = 0; // the deepest folder wins: a game inside a launcher's own folder is the game
    for (size_t i = 0; i < games.size(); ++i) {
        const Game& g = games[i];
        if (!g.dir.empty() && g.dir.size() > depth && exeLower.compare(0, g.dir.size(), g.dir) == 0) best = (int)i, depth = g.dir.size();
    }
    return best;
}

HWND Alive(const std::vector<Game>& games, int game) {
    if (game < 0 || game >= (int)games.size()) return nullptr;
    struct Ask { const std::vector<Game>* games; int game; HWND found; DWORD lastPid; bool lastHit; } ask{&games, game, nullptr, 0, false};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        Ask& a = *reinterpret_cast<Ask*>(lp);
        if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE; // a window of its own on the taskbar, minimized or not
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != a.lastPid) a.lastPid = pid, a.lastHit = Match(*a.games, Lower(ExeOf(pid))) == a.game; // a program's windows come together: asked once
        if (a.lastHit) a.found = w;
        return !a.found;
    }, reinterpret_cast<LPARAM>(&ask));
    return ask.found;
}

bool Sniff(const std::wstring& exe, HWND win, Game& out) {
    std::wstring low = Lower(exe);
    std::replace(low.begin(), low.end(), L'/', L'\\');
    size_t slash = low.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    for (const wchar_t* no : kNotGames)
        if (low.find(no) != std::wstring::npos) return false;
    std::wstring file = low.substr(slash + 1), dir = exe.substr(0, slash);
    for (const wchar_t* helper : kHelpers)
        if (file.find(helper) != std::wstring::npos) return false;
    if (std::any_of(std::begin(kJavaFiles), std::end(kJavaFiles), [&](const wchar_t* j) { return file == j; })) { // java runs anything: only its game windows count
        wchar_t cls[64] = {}, title[160] = {};
        GetClassNameW(win, cls, 64);
        if (std::none_of(std::begin(kGameWindows), std::end(kGameWindows), [&](const wchar_t* g) { return wcscmp(cls, g) == 0; })) return false;
        GetWindowTextW(win, title, 160);
        std::wstring name = title;
        size_t cut = name.find_first_of(L"0123456789*-("); // "Minecraft 1.21.4 - Singleplayer": the name ends where the version begins
        if (cut != std::wstring::npos && cut > 2) name.resize(cut);
        while (!name.empty() && name.back() == L' ') name.pop_back();
        if (name.empty()) name = L"Java game";
        out = Game{};
        out.id = L"java:" + Lower(name), out.name = name, out.exe = low, out.source = kSniffed;
        return true;
    }
    bool engine = file.size() > wcslen(kUnrealTail) && file.compare(file.size() - wcslen(kUnrealTail), std::wstring::npos, kUnrealTail) == 0;
    for (size_t i = 0; i < std::size(kEngineFiles) && !engine; ++i) engine = Exists(dir + L"\\" + kEngineFiles[i]);
    if (!engine) return false;
    out = Marked(exe);
    out.source = kSniffed;
    return true;
}

}
