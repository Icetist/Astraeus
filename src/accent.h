#pragma once
#include <windows.h>
#include <memory>

namespace sys {

class WinAccent { // windows' own accent color, and word when it changes; listens only while asked to
public:
    WinAccent();
    ~WinAccent();
    WinAccent(const WinAccent&) = delete;
    WinAccent& operator=(const WinAccent&) = delete;
    int Read(); // 0xRRGGBB, -1 if windows won't say
    void Watch(HWND notify, UINT msg); // posts msg whenever the accent changes
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
