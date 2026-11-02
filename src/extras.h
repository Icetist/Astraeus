#pragma once
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// small readers for the pill and the tools page: the keyboard's language, the window in front, the system drive, the recycle bin,
// pending updates and a few hosts' health. The cheap ones are plain calls; the slow ones run on a worker and post back.
namespace sys::extras {

std::wstring Language(); // "EN": the language the app in front types in
void NextLanguage(); // that app switches to the next keyboard, as win+space would
std::wstring ActiveTitle(); // the front window's title, empty when it's ours or untitled
bool SystemDrive(float& used, double& freeGb); // the windows drive: 0..1 full, and what's left

struct BinInfo {
    long long bytes = 0, items = 0;
};
BinInfo Bin();
void EmptyBin(HWND owner); // windows asks first, as it does from the desktop

struct UpdateCount {
    int system = -1, apps = -1; // windows' own and installed packages; -1 not known yet
    bool busy = false;
};

class Updates { // looked for on a worker: windows update can take a minute
public:
    Updates();
    ~Updates();
    void Check(HWND notify, UINT msg); // posts msg when done; a check already running is left to finish
    UpdateCount Get() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

struct HostHealth {
    std::wstring host;
    bool up = false, known = false;
    int ms = -1;
};

class Pinger { // each host asked once a minute while it runs: ping for names, a request for web addresses
public:
    Pinger();
    ~Pinger();
    Pinger(const Pinger&) = delete;
    Pinger& operator=(const Pinger&) = delete;
    void Start(HWND notify, UINT msg, const std::vector<std::wstring>& hosts); // again with a new list restarts it
    void Stop();
    bool On() const;
    void Now(); // look again right away
    std::vector<HostHealth> Results() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
