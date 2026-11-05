#pragma once
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// Workspaces from whichever tiling manager runs, else windows' own virtual desktops. Holds nothing until Start; a worker waits on events, nothing polls.
namespace sys {

struct Workspace {
    std::wstring name;
    bool busy = false; // has windows on it
};

struct WsState {
    enum Source { kNone, kDesktops, kTiling, kTilingWs, kTilingCli }; // windows' virtual desktops, a tiling manager over a pipe, a socket or its command line
    int source = kNone;
    std::vector<Workspace> list;
    int current = -1;
};

enum class Tiler { kNone, kPipe, kSocket, kCli, kFancy, kAmethyst }; // which tiling manager this pc has; the last two tile on windows' own virtual desktops, read as desktops
Tiler FindTiler(); // a quick look: running now, or installed where they usually go; one process list and a few file checks, no scan
inline bool TilesOnDesktops(Tiler t) { return t == Tiler::kFancy || t == Tiler::kAmethyst; }
const wchar_t* TilerName(Tiler t); // what the tile's hover calls it

class Workspaces {
public:
    Workspaces();
    ~Workspaces();
    Workspaces(const Workspaces&) = delete;
    Workspaces& operator=(const Workspaces&) = delete;
    void Start(HWND notify, UINT msg); // msg is posted whenever the list or the current one changes
    void Stop();
    bool On() const;
    WsState Snap() const;
    void Focus(int index); // switch to that workspace
    void Step(int delta); // the next or previous one, wrapping

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
