#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct ShareOut {
    std::wstring id, name;
    bool main = false; // the default output, everything else copies it
    bool on = false; // also playing here
    bool bt = false, phones = false;
    bool live = false; // stream running right now (off while idling on silence)
    bool failed = false; // couldn't open or was lost
    int delayMs = 0; // user sync nudge, added on top of the automatic alignment
    float volume = 1; // 0..1, this device only
};

class Share { // plays the default output on extra devices in sync; holds no thread unless something is shared
public:
    Share();
    ~Share();
    Share(const Share&) = delete;
    Share& operator=(const Share&) = delete;
    void Notify(HWND hwnd, UINT msg); // posted when an output's state changes
    std::vector<ShareOut> List() const; // active outputs, hands-free (call quality) endpoints left out
    void Enable(const std::wstring& id, bool on);
    void SetDelay(const std::wstring& id, int ms);
    void SetVolume(const std::wstring& id, float v, bool persist = true); // persist=false while dragging
    bool Active() const; // anything shared
    void StopAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
