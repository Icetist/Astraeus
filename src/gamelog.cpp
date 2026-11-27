// A session belongs to a game, not to one program: it starts when a game's window comes to the front and ends once the game has
// had no window for a few seconds, so a starter that hands over to the real game, or a launcher inside the game's own folder,
// is all one session. Everything is read from this pc; the history is a small text file beside the shelf's.
#include "gamelog.h"

#include <shobjidl.h>

#include <algorithm>
#include <ctime>
#include <iterator>
#include <mutex>
#include <thread>

#include "shelf_impl.h"
#include "sys.h"

namespace shelfio = sys::shelfio; // the shelf's file helpers: the same folder, the same safe writes

namespace {

constexpr int kFirstScanSec = 20; // the launchers' records are read a little after start, never while astraeus is coming up
constexpr int kAliveEvery = 3; // seconds between looks for the game's windows while it's not in front
constexpr int kGoneSec = 9; // this long with no window and the game is closed
constexpr int kKeepSec = 30; // a session shorter than this was a launcher passing through: not kept
constexpr int kSwitchSec = 5; // another game in front this long is the one being played; a quick alt-tab isn't
constexpr size_t kKeepLines = 1000; // the history's length, oldest dropped
constexpr size_t kVerdictCap = 512; // programs remembered as game or not, cleared when it fills
constexpr size_t kHistoryMax = 1u << 20;
constexpr int kDaySec = 86400, kWeekDays = 7;
constexpr int kMissDays = 7; // a game with no cover anywhere is asked again after this: igdb keeps adding them
constexpr wchar_t kMarkedVal[] = L"GamesMarked", kIgnoredVal[] = L"GamesIgnored";
constexpr const wchar_t* kJava[] = {L"\\javaw.exe", L"\\java.exe"};

std::wstring HistoryPath() {
    std::wstring dir = shelfio::Dir();
    return dir.empty() ? dir : dir + L"\\games.txt";
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t at = 0;
    while (at <= s.size()) {
        size_t next = s.find(sep, at);
        if (next == std::wstring::npos) next = s.size();
        out.push_back(s.substr(at, next - at));
        at = next + 1;
    }
    return out;
}

std::wstring Join(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& p : parts) out += (out.empty() ? L"" : L"\n") + p;
    return out;
}

std::wstring Clean(std::wstring s) { // one field of a line: no tabs or breaks inside it
    std::replace_if(s.begin(), s.end(), [](wchar_t c) { return c == L'\t' || c == L'\n' || c == L'\r'; }, L' ');
    return s;
}

bool IsJava(const std::wstring& exe) {
    return std::any_of(std::begin(kJava), std::end(kJava), [&](const wchar_t* j) { size_t n = wcslen(j); return exe.size() > n && exe.compare(exe.size() - n, n, j) == 0; });
}

bool ParseLine(const std::wstring& line, GameLog::Session& s) { // start, seconds, in front, clips, id, name, program
    std::vector<std::wstring> f = Split(line, L'\t');
    if (f.size() < 6) return false;
    s.start = _wtoi64(f[0].c_str()), s.secs = _wtoi(f[1].c_str()), s.focus = _wtoi(f[2].c_str()), s.clips = _wtoi(f[3].c_str());
    s.id = f[4], s.name = f[5], s.exe = f.size() > 6 ? f[6] : L"";
    return !s.id.empty() && s.start > 0;
}

int64_t MidnightToday() { // local time
    time_t t = time(nullptr);
    tm local{};
    localtime_s(&local, &t);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    return (int64_t)mktime(&local);
}

bool AnyWindowOf(const std::wstring& exe) { // a window of its own still open by that program
    struct Ask { const std::wstring* exe; bool found; DWORD lastPid; bool lastHit; } ask{&exe, false, 0, false};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        Ask& a = *reinterpret_cast<Ask*>(lp);
        if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != a.lastPid) a.lastPid = pid, a.lastHit = sys::games::Lower(sys::games::ExeOf(pid)) == *a.exe;
        a.found = a.lastHit;
        return !a.found;
    }, reinterpret_cast<LPARAM>(&ask));
    return ask.found;
}

}

std::wstring GameLog::Span(int secs) {
    constexpr int kMin = 60, kHour = 3600;
    if (secs < kMin) return std::to_wstring((std::max)(secs, 0)) + L" s";
    if (secs < kHour) return std::to_wstring(secs / kMin) + L" min";
    int m = secs % kHour / kMin;
    return std::to_wstring(secs / kHour) + L" h" + (m ? L" " + std::to_wstring(m) + L" min" : L"");
}

struct GameLog::Shared {
    std::mutex mu;
    std::vector<Game> result;
    bool busy = false, fresh = false;
    struct Ask {
        std::wstring id, name, key; // key: the cover found, empty when none
        int result = sys::games::kArtNone;
    };
    std::vector<Ask> asks, answers; // covers to look for, and what came back
    bool asking = false;
};

void GameLog::Start(HWND to, UINT libraryMsg) {
    notify = to, msg = libraryMsg;
    shared = std::make_shared<Shared>();
    for (const auto& line : Split(sys::ReadText(kMarkedVal), L'\n'))
        if (!line.empty()) marked.push_back(line);
    for (const auto& line : Split(sys::ReadText(kIgnoredVal), L'\n'))
        if (!line.empty()) ignored.insert(line);
    LoadHistory();
    LoadArt();
    Compose();
}

void GameLog::LoadArt() { // id, the name it was given, the cover's key, tried, preferred, when a miss was asked
    std::string bytes;
    std::wstring dir = shelfio::Dir();
    if (dir.empty() || !shelfio::ReadAll(dir + L"\\gameart.txt", bytes, kHistoryMax)) return;
    int64_t nowT = (int64_t)time(nullptr);
    for (auto& line : Split(shelfio::Wide(bytes), L'\n')) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> f = Split(line, L'\t');
        if (f.size() < 4 || f[0].empty()) continue;
        Art a{f[1], f[2], f[3] == L"1", f.size() > 4 && f[4] == L"1", f.size() > 5 ? _wtoi64(f[5].c_str()) : 0};
        if (a.appid.empty()) a.tried = a.asked > 0 && nowT - a.asked < (int64_t)kMissDays * kDaySec; // a miss stands for a week
        art[f[0]] = std::move(a);
    }
}

void GameLog::SaveArt() const {
    std::wstring dir = shelfio::Dir();
    if (dir.empty()) return;
    std::wstring all;
    for (const auto& [id, a] : art)
        all += Clean(id) + L"\t" + Clean(a.name) + L"\t" + Clean(a.appid) + L"\t" + (a.tried ? L"1" : L"0") + L"\t" + (a.prefer ? L"1" : L"0") + L"\t" + std::to_wstring(a.asked) + L"\n";
    CreateDirectoryW(dir.c_str(), nullptr);
    shelfio::WriteAtomic(dir + L"\\gameart.txt", shelfio::Utf8(all));
}

void GameLog::Fetch(const std::wstring& id, const std::wstring& name) {
    if (!shared || !asking.insert(id).second) return;
    std::lock_guard<std::mutex> lock(shared->mu);
    shared->asks.push_back({id, name});
    if (shared->asking) return; // the worker running takes it too
    shared->asking = true;
    std::thread([s = shared, to = notify, m = msg] { // one worker, through the queue in turn: steam and igdb are asked gently
        for (;;) {
            Shared::Ask a;
            {
                std::lock_guard<std::mutex> lock(s->mu);
                if (s->asks.empty()) {
                    s->asking = false;
                    return;
                }
                a = std::move(s->asks.front());
                s->asks.erase(s->asks.begin());
            }
            std::wstring file;
            a.result = sys::games::FindArt(a.id, a.name, a.key, file);
            {
                std::lock_guard<std::mutex> lock(s->mu);
                s->answers.push_back(std::move(a));
            }
            PostMessageW(to, m, 0, 0);
        }
    }).detach();
}

void GameLog::TakeArt() {
    std::vector<Shared::Ask> got;
    {
        std::lock_guard<std::mutex> lock(shared->mu);
        got.swap(shared->answers);
    }
    if (got.empty()) return;
    for (const auto& a : got) {
        asking.erase(a.id);
        if (a.result == sys::games::kArtOffline) continue; // asked again on the next scan
        Art& e = art[a.id];
        bool found = a.result == sys::games::kArtFound;
        e.appid = found ? a.key : L"", e.tried = true; // a picture that didn't download is no cover
        e.asked = found ? 0 : (int64_t)time(nullptr);
    }
    ++artEpoch;
    SaveArt();
}

void GameLog::Dress(Game& g) const {
    auto it = art.find(g.id);
    if (it == art.end()) return;
    if (!it->second.name.empty()) g.name = it->second.name;
    std::wstring file = sys::games::CoverFile(it->second.appid);
    if (!file.empty() && (g.cover.empty() || !it->second.name.empty() || it->second.prefer) && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) g.cover = file; // renamed: the new name's cover wins over the launcher's
}

void GameLog::Rename(const std::wstring& id, const std::wstring& name) {
    std::wstring n = name;
    while (!n.empty() && n.back() == L' ') n.pop_back();
    while (!n.empty() && n.front() == L' ') n.erase(n.begin());
    Art& e = art[id];
    e.name = n, e.appid.clear(), e.tried = false;
    SaveArt();
    totalsDirty = true;
    asking.erase(id); // a lookup for the old name may still come back: the new one is asked after it
    Compose();
}

void GameLog::Refetch(const Game& g) {
    Art& e = art[g.id];
    std::wstring old = sys::games::CoverFile(e.appid);
    if (!old.empty()) DeleteFileW(old.c_str()); // a broken or wrong picture isn't reused
    e.appid.clear(), e.tried = false, e.prefer = true, e.asked = 0;
    SaveArt();
    asking.erase(g.id);
    ++artEpoch;
    Compose(); // asks for it: untried and preferred
}

const std::vector<int>& GameLog::Order(int sort) {
    const auto& t = Totals(); // fresh first: it marks the order stale when the history moved
    if (!orderDirty && sort == orderSort) return order;
    orderDirty = false, orderSort = sort;
    order.resize(lib.size());
    for (size_t i = 0; i < lib.size(); ++i) order[i] = (int)i; // lib is by name already
    if (sort == kByName) return order;
    auto key = [&](int i) -> int64_t {
        auto it = totalAt.find(lib[(size_t)i].id);
        if (it == totalAt.end()) return -1; // never played: after every played one
        return sort == kByRecent ? t[it->second].last : t[it->second].all;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key(a) > key(b); }); // stable: ties and the unplayed keep the name order
    return order;
}

void GameLog::Rescan() {
    if (!shared) return;
    {
        std::lock_guard<std::mutex> lock(shared->mu);
        if (shared->busy) return;
        shared->busy = true;
    }
    scanAt = sinceStart;
    std::thread([s = shared, to = notify, m = msg] { // holds the shared part: astraeus closing mid-scan leaves it nothing to write into
        std::vector<Game> found = sys::games::Scan();
        {
            std::lock_guard<std::mutex> lock(s->mu);
            s->result = std::move(found);
            s->busy = false, s->fresh = true;
        }
        PostMessageW(to, m, 0, 0);
    }).detach();
}

bool GameLog::Scanning() const {
    if (!shared) return false;
    std::lock_guard<std::mutex> lock(shared->mu);
    return shared->busy;
}

void GameLog::OnLibrary() {
    if (!shared) return;
    TakeArt();
    {
        std::lock_guard<std::mutex> lock(shared->mu);
        if (shared->fresh) scanned = std::move(shared->result);
        shared->fresh = false;
    }
    Compose();
}

void GameLog::Compose() {
    lib.clear();
    for (const Game& g : scanned)
        if (!ignored.count(g.id)) lib.push_back(g);
    for (const auto& exe : marked) {
        Game g = sys::games::Marked(exe);
        if (!ignored.count(g.id) && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) lib.push_back(std::move(g));
    }
    for (Game& g : lib) {
        Dress(g);
        auto it = art.find(g.id);
        bool tried = it != art.end() && it->second.tried;
        bool wanted = g.cover.empty() || (it != art.end() && (!it->second.name.empty() || it->second.prefer));
        if (!tried && wanted && sinceStart >= kFirstScanSec) Fetch(g.id, g.name); // a missing cover, a new name's, or one asked for again
    }
    std::sort(lib.begin(), lib.end(), [](const Game& a, const Game& b) { return sys::games::Lower(a.name) < sys::games::Lower(b.name); });
    libAt.clear();
    for (size_t i = 0; i < lib.size(); ++i) {
        const Game& g = lib[i];
        libAt[g.id] = i;
        if (g.id == last.id) last.name = g.name, last.cover = g.cover; // the card on top shows it as the list does
    }
    orderDirty = true;
    verdicts.clear();
    lastFg = nullptr; // what's in front is asked again against the new list
}

bool GameLog::Resolve(HWND w, Game& out, std::wstring& exe) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    exe = sys::games::Lower(sys::games::ExeOf(pid));
    if (exe.empty()) return false;
    auto it = verdicts.find(exe);
    if (it == verdicts.end() || it->second.again) {
        Verdict v;
        v.lib = sys::games::Match(lib, exe);
        v.again = IsJava(exe);
        if (v.lib >= 0) v.game = true;
        else if (Game g; sys::games::Sniff(exe, w, g) && !ignored.count(g.id)) v.game = true, v.found = std::move(g);
        if (verdicts.size() >= kVerdictCap) verdicts.clear();
        it = verdicts.insert_or_assign(exe, std::move(v)).first;
    }
    if (!it->second.game) return false;
    out = it->second.lib >= 0 ? lib[(size_t)it->second.lib] : it->second.found;
    return true;
}

int GameLog::Tick(bool detect) {
    ++sinceStart;
    if (sinceStart == kFirstScanSec) Rescan();
    if (!detect) {
        EndNow();
        return kNone;
    }
    HWND fg = GetForegroundWindow();
    Game g;
    std::wstring exe;
    bool fresh = false;
    if (fg != lastFg) {
        lastFg = fg;
        fgId.clear();
        if (fg && Resolve(fg, g, exe)) fgId = g.id, fresh = true;
    }
    if (!playing) {
        if (fgId.empty()) return kNone;
        if (!fresh) { // in front since before: asked again next second, with its window
            lastFg = nullptr;
            return kNone;
        }
        Begin(g, exe, fg);
        return kStarted;
    }
    ++now.secs;
    if (fgId == now.id) {
        ++now.focus;
        gone = 0, otherFor = 0;
        if (fresh) nowWin = fg; // the starter's window gave way to the game's
        return kNone;
    }
    otherFor = fgId.empty() ? 0 : otherFor + 1;
    if (otherFor >= kSwitchSec && Resolve(fg, g, exe)) { // two games open: the one played now has the session, the other's is kept as it was
        End();
        Begin(g, exe, fg);
        return kStarted;
    }
    if (now.secs % kAliveEvery == 0) gone = Alive() ? 0 : gone + kAliveEvery;
    if (gone < kGoneSec) return kNone;
    bool kept = now.secs >= kKeepSec;
    End();
    return kept ? kEnded : kNone;
}

void GameLog::Begin(const Game& g, const std::wstring& exe, HWND w) {
    now = Session{};
    now.id = g.id, now.name = g.name, now.exe = exe, now.cover = g.cover, now.source = g.source;
    now.start = (int64_t)time(nullptr);
    playing = true, gone = 0, otherFor = 0, nowWin = w;
}

void GameLog::End() {
    playing = false;
    nowWin = nullptr;
    lastFg = nullptr; // another game in front now starts its own session
    if (now.secs < kKeepSec) return;
    last = now;
    Append(now);
}

void GameLog::EndNow() {
    if (playing) End();
}

bool GameLog::Alive() {
    if (IsJava(now.exe)) return nowWin && IsWindow(nowWin) && IsWindowVisible(nowWin); // java runs other things too: only the game's own window counts
    for (size_t i = 0; i < lib.size(); ++i)
        if (lib[i].id == now.id) return sys::games::Alive(lib, (int)i) != nullptr;
    return AnyWindowOf(now.exe); // told by what it is: its own program's windows
}

void GameLog::LoadHistory() {
    history.clear();
    std::string bytes;
    std::wstring path = HistoryPath();
    if (path.empty() || !shelfio::ReadAll(path, bytes, kHistoryMax)) return;
    for (auto& line : Split(shelfio::Wide(bytes), L'\n')) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) history.push_back(std::move(line));
    }
    Session s;
    if (!history.empty() && ParseLine(history.back(), s)) last = s;
    totalsDirty = true;
}

void GameLog::Append(const Session& s) {
    history.push_back(std::to_wstring(s.start) + L"\t" + std::to_wstring(s.secs) + L"\t" + std::to_wstring(s.focus) + L"\t" + std::to_wstring(s.clips) + L"\t" + Clean(s.id) + L"\t" + Clean(s.name) + L"\t" + Clean(s.exe));
    if (history.size() > kKeepLines) history.erase(history.begin(), history.begin() + (ptrdiff_t)(history.size() - kKeepLines));
    SaveHistory();
}

void GameLog::SaveHistory() {
    totalsDirty = true;
    std::wstring path = HistoryPath();
    if (path.empty()) return;
    CreateDirectoryW(shelfio::Dir().c_str(), nullptr); // there already when the shelf has been used
    std::wstring all;
    for (const auto& line : history) all += line + L"\n";
    shelfio::WriteAtomic(path, shelfio::Utf8(all));
}

const std::vector<GameLog::Total>& GameLog::Totals() {
    if (!totalsDirty) return totals;
    totalsDirty = false;
    totals.clear();
    int64_t today = MidnightToday(), week = today - (int64_t)(kWeekDays - 1) * kDaySec;
    std::unordered_map<std::wstring, size_t> at;
    for (const auto& line : history) {
        Session s;
        if (!ParseLine(line, s)) continue;
        auto [it, added] = at.try_emplace(s.id, totals.size());
        if (added) totals.push_back(Total{s.id, s.name, s.exe});
        Total& t = totals[it->second];
        t.name = s.name; // the newest name wins: a launcher can rename a game
        if (!s.exe.empty()) t.exe = s.exe;
        t.all += s.secs, ++t.sessions, t.last = (std::max)(t.last, s.start);
        if (s.start >= today) t.today += s.secs;
        if (s.start >= week) t.week += s.secs;
    }
    for (Total& t : totals) {
        auto it = art.find(t.id);
        if (it != art.end() && !it->second.name.empty()) t.name = it->second.name; // renamed: its past sessions too
    }
    std::sort(totals.begin(), totals.end(), [](const Total& a, const Total& b) { return a.all > b.all; });
    totalAt.clear();
    for (size_t i = 0; i < totals.size(); ++i) totalAt[totals[i].id] = i;
    orderDirty = true;
    return totals;
}

const GameLog::Total* GameLog::TotalOf(const std::wstring& id) {
    const auto& t = Totals();
    auto it = totalAt.find(id);
    return it == totalAt.end() ? nullptr : &t[it->second];
}

void GameLog::SaveLists() const {
    sys::SaveText(kMarkedVal, Join(marked));
    sys::SaveText(kIgnoredVal, Join(std::vector<std::wstring>(ignored.begin(), ignored.end())));
}

void GameLog::Mark(const std::wstring& exe) {
    std::wstring low = sys::games::Lower(exe);
    std::replace(low.begin(), low.end(), L'/', L'\\');
    if (low.empty()) return;
    ignored.erase(L"exe:" + low); // hidden once, added back by hand: it counts
    if (std::find(marked.begin(), marked.end(), low) == marked.end()) marked.push_back(low);
    SaveLists();
    Compose();
}

std::wstring GameLog::PickProgram(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    COMDLG_FILTERSPEC types[] = {{L"Programs", L"*.exe"}};
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(L"Add a game to Astraeus");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    std::wstring out;
    IShellItem* item = nullptr;
    PWSTR path = nullptr;
    if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    if (item) item->Release();
    dlg->Release();
    return out;
}

void GameLog::Forget(const Game& g) {
    auto was = std::find(marked.begin(), marked.end(), g.exe);
    if (g.source == sys::games::kMarked && was != marked.end()) marked.erase(was);
    else ignored.insert(g.id);
    SaveLists();
    Compose();
}

void GameLog::Unhide() {
    ignored.clear();
    SaveLists();
    Compose();
}

std::vector<GameLog::Game> GameLog::HiddenGames() {
    std::vector<Game> out;
    for (const auto& id : ignored) {
        auto known = std::find_if(scanned.begin(), scanned.end(), [&](const Game& g) { return g.id == id; });
        if (known != scanned.end()) {
            out.push_back(*known);
            continue;
        }
        Game g;
        g.id = id;
        if (const Total* t = TotalOf(id)) g.name = t->name, g.exe = t->exe;
        else if (id.rfind(L"exe:", 0) == 0) g.exe = id.substr(4), g.name = sys::games::ProductName(g.exe);
        else g.name = id.substr(id.find(L':') + 1); // a launcher game since uninstalled: its id is all that's left
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return sys::games::Lower(a.name) < sys::games::Lower(b.name); });
    return out;
}

void GameLog::Remove(const std::wstring& id) {
    ignored.erase(id);
    SaveLists();
    size_t before = history.size();
    history.erase(std::remove_if(history.begin(), history.end(), [&](const std::wstring& line) {
        Session s;
        return ParseLine(line, s) && s.id == id;
    }), history.end());
    if (history.size() != before) SaveHistory();
    if (last.id == id) last = Session{};
    if (art.erase(id)) SaveArt(); // its name and cover go too
    Compose();
}
