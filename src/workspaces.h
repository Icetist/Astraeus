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
    enum Source { kNone, kDesktops, kTiling, kTilingWs }; // windows' virtual desktops, a tiling manager over a pipe or over a socket
    int source = kNone;
    std::vector<Workspace> list;
    int current = -1;
};

enum class Tiler { kNone, kPipe, kSocket }; // which tiling manager this pc has: the pipe one or the socket one
Tiler FindTiler(); // a quick look: running now, or installed where they usually go; a few file checks, no scan
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
