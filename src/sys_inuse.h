#pragma once
#include <windows.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Which apps have the microphone or the camera right now, as Windows' own privacy settings record it. Nothing polls: a
// thread sleeps on the registry and wakes only when an app starts or stops using one, then tells the window.
namespace sys::inuse {

struct User {
    std::wstring name; // what to call the app
    bool mic = false, cam = false;
};

class Watch {
public:
    ~Watch() { Stop(); }
    void Start(HWND notify, UINT msg); // msg is posted whenever who's using them changes
    void Stop();
    std::vector<User> Now() const; // astraeus itself is never in it
    bool On() const { return run.joinable(); }

private:
    void Run();
    void Scan();
    HWND hwnd = nullptr;
    UINT message = 0;
    HANDLE quit = nullptr;
    std::thread run;
    mutable std::mutex mu;
    std::vector<User> users;
};

}
