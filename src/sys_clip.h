#pragma once
#include <windows.h>

#include <memory>
#include <string>

namespace sys::clip { // the game's window, and nothing else, recorded into memory: the last stretch is kept, and a clip is its end written out as an mp4

struct Options {
    int fps = 60;
    int quality = 1; // 0 light, 1 balanced, 2 high
    int height = 0; // 0: the game's own size, else scaled down to this
    int audio = 1; // 0 none, 1 the game's own sound, 2 everything but astraeus
    bool cursor = false; // the mouse pointer drawn in
    bool mic = false; // the default microphone mixed into the sound
    int keepSecs = 30; // the longest a clip can be
};

class Recorder {
public:
    static std::unique_ptr<Recorder> Start(HWND game, DWORD pid, const Options& o, std::wstring& why); // null, with why in plain words, when the game can't be recorded
    virtual ~Recorder() = default;
    virtual void Pause(bool on) = 0; // paused, nothing is kept: a clip after resuming starts there
    virtual bool Paused() const = 0;
    virtual bool Follow(HWND w) = 0; // the game moved to another window: that one is recorded from now
    virtual bool Alive() const = 0; // false once the window is gone or capture stopped
    virtual size_t Bytes() const = 0; // memory the kept stretch holds
    virtual double Seconds() const = 0; // how much is kept so far
    virtual bool HardwareEncoder() const = 0; // false: the cpu encodes
    // the last secs written to path off the ui thread, plus the after seconds still to come (recording goes on meanwhile); then msg
    // goes to notify with wparam 1 saved or 0 not and lparam a new std::wstring* (the path, or why not) for the receiver to delete
    virtual void Save(int secs, int after, const std::wstring& path, HWND notify, UINT msg) = 0;
};

bool Supported(); // windows here can capture a single window

}
