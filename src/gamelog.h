#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "games.h"

class GameLog { // which game is being played, for how long, and what was played before. all of it stays on this pc
public:
    using Game = sys::games::Game;
    struct Session {
        std::wstring id, name, exe, cover; // exe: the program that was in front, for its icon
        int source = sys::games::kSteam;
        int64_t start = 0; // unix seconds
        int secs = 0, focus = 0, clips = 0; // how long it ran, how long it was in front, clips kept
    };
    struct Total { // one game, summed over the history
        std::wstring id, name, exe;
        int today = 0, week = 0, all = 0, sessions = 0; // seconds played
        int64_t last = 0;
    };
    enum Event { kNone, kStarted, kEnded };
    static std::wstring Span(int secs); // "1 h 12 min", "8 min", "40 s"

    GameLog() = default;
    GameLog(const GameLog&) = delete;
    GameLog& operator=(const GameLog&) = delete;

    void Start(HWND notify, UINT libraryMsg); // reads the history; the library is looked for a little later
    void Rescan(); // the launchers' records read again, off the ui thread; the message says when it's in
    void OnLibrary(); // ui thread: the new list taken in
    int Tick(bool detect); // once a second on the ui thread: kStarted or kEnded on the second it happens
    void EndNow(); // detection was switched off: a session going on ends here

    bool Playing() const { return playing; }
    HWND Window() const { return nowWin; } // the game's window now: the recorder follows it
    void AddClip() { if (playing) ++now.clips; }
    const Session& Now() const { return now; }
    bool HasLast() const { return !last.id.empty(); }
    const Session& Last() const { return last; }
    const std::vector<Game>& Library() const { return lib; }
    bool Scanning() const;
    const std::vector<Total>& Totals(); // most played first; worked out again only when the history changed
    const Total* TotalOf(const std::wstring& id);
    enum Sort { kByName, kByRecent, kByTime };
    const std::vector<int>& Order(int sort); // the library's indices in that order; never played ones last, by name, as steam does
    bool Installed(const std::wstring& id) const { return libAt.count(id) != 0; }
    const Game* GameOf(const std::wstring& id) const { auto it = libAt.find(id); return it == libAt.end() ? nullptr : &lib[it->second]; }
    int ArtEpoch() const { return artEpoch; } // goes up when a cover arrives: pictures drawn from the old file are dropped

    void Mark(const std::wstring& exe, const std::wstring& name = L""); // a program counted as a game from now on, whatever it is; name: what its window is called
    static std::wstring PickProgram(HWND owner); // windows' file picker for a program; empty when cancelled
    void Forget(const Game& g); // not a game: one added by hand is unmarked, any other hidden
    void Unhide(); // every hidden one comes back
    size_t Hidden() const { return ignored.size(); }
    void Rename(const std::wstring& id, const std::wstring& name); // empty: its own name back. steam's cover for the new name is looked for
    void Refetch(const Game& g); // its cover looked for again, and preferred to the launcher's own once found
    std::vector<Game> HiddenGames(); // named from the last scan or the history, by name
    void Remove(const std::wstring& id); // gone for good: unhidden and its time wiped, so a scan that finds it again counts it anew

private:
    struct Verdict {
        int lib = -1; // index into lib, -1 none
        bool game = false;
        bool again = false; // asked again each time: java's verdict hangs on the window, not the program
        Game found; // a game no library lists, told by what it is
    };
    bool Resolve(HWND w, Game& out, std::wstring& exe);
    void Begin(const Game& g, const std::wstring& exe, HWND w);
    void End();
    bool Alive();
    void Compose(); // the library: the scan, plus what's marked by hand, less what's hidden
    void LoadHistory();
    void Append(const Session& s);
    void SaveHistory();
    struct Art {
        std::wstring name, appid; // the name it was given, and steam's id for its cover
        bool tried = false; // asked already: a cover found isn't asked again, a miss only after kMissDays
        bool prefer = false; // fetched on request: wins over the launcher's own picture
        int64_t asked = 0; // when a miss was last asked, unix seconds
    };
    void LoadArt();
    void SaveArt() const;
    void Fetch(const std::wstring& id, const std::wstring& name); // queued for the worker
    void TakeArt(); // ui thread: what the worker found
    void Dress(Game& g) const; // its new name and fetched cover put on
    std::unordered_map<std::wstring, Art> art; // by game id
    std::unordered_set<std::wstring> asking; // queued or being asked
    std::unordered_map<std::wstring, size_t> totalAt; // id to its place in totals: a row finds its time without a search
    std::unordered_map<std::wstring, size_t> libAt; // what's installed, by id: most played and the menu find theirs at once
    std::vector<int> order;
    int orderSort = -1;
    bool orderDirty = true;
    int artEpoch = 0;
    void SaveLists() const;

    HWND notify = nullptr;
    UINT msg = 0;
    struct Shared;
    std::shared_ptr<Shared> shared; // the scan's result, handed over from the worker
    std::vector<Game> scanned, lib;
    std::vector<std::wstring> marked; // lowercase program paths
    std::unordered_set<std::wstring> ignored; // game ids
    std::unordered_map<std::wstring, Verdict> verdicts; // by lowercase program path
    std::vector<std::wstring> history; // utf-16 lines, oldest first
    std::vector<Total> totals;
    bool totalsDirty = true;
    bool playing = false;
    Session now, last;
    HWND nowWin = nullptr, lastFg = nullptr;
    std::wstring fgId; // the game in front, empty: none
    int sinceStart = 0, gone = 0, scanAt = 0;
    int otherFor = 0; // seconds another game has been in front while this one runs
};
