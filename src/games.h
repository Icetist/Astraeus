#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace sys::games { // which programs on this pc are games, read from what the launchers already wrote down; only a missing cover is fetched, from steam's store

enum Source { kSteam, kEpic, kGog, kUbisoft, kXbox, kRiot, kWindows, kSniffed, kMarked, kSourceCount }; // kWindows: its own list of what it took for a game; kSniffed: told by what it is, in no list; kMarked: by hand, on the games page
const wchar_t* SourceName(int source);

struct Game {
    std::wstring id; // steady across scans: the source and its own id for the game
    std::wstring name;
    std::wstring dir; // lowercase, ends with a backslash: every program under it is this game. empty when only exe counts
    std::wstring exe; // lowercase full path, when one program is the game
    std::wstring cover; // a picture of it already on disk, or empty: the program's own icon then
    int source = kSteam;
};

std::vector<Game> Scan(); // every library found; reads files and the registry, so off the ui thread
int Match(const std::vector<Game>& games, const std::wstring& exeLower); // which game that program belongs to, -1 for none; helpers (crash reporters, installers) never count
HWND Alive(const std::vector<Game>& games, int game); // a window that game still has open, null when none is left: a starter or its launcher hands over to the game itself and the session goes on, a launcher left in the tray doesn't hold it open
bool Sniff(const std::wstring& exe, HWND win, Game& out); // a program in no library that is a game all the same: a java game's window, or an engine's files beside it
std::wstring Lower(std::wstring s);
std::wstring ExeOf(DWORD pid); // full path of a running program, empty if it can't be asked
std::wstring ProductName(const std::wstring& exe); // what the program calls itself, or its file name without .exe
Game Marked(const std::wstring& exe); // a game made from one program, for one marked by hand

enum ArtResult { kArtFound, kArtNone, kArtOffline };
int FindArt(const std::wstring& id, const std::wstring& name, std::wstring& appid, std::wstring& file); // steam's cover for it, by its steam id or its name; blocks on the network, so off the ui thread
std::wstring CoverFile(const std::wstring& appid); // where a fetched cover is kept

}
