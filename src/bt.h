#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys {

enum BtKind { kBtOther, kBtAudio, kBtKeys, kBtMouse, kBtPhone, kBtPc, kBtPad, kBtWatch };

struct BtDev {
    std::wstring id, name, mac; // mac = 12 lowercase hex, matches audio driver ids
    int kind = kBtOther;
    bool paired = false, on = false; // on = connected
    bool audio = false; // audio driver handles connect
    bool le = false; // bluetooth low energy, else classic
    bool want = false;
    uint64_t until = 0; // pending op deadline, tick ms
    bool pairing = false;
    int battery = -1; // percent as windows reports it, -1 unknown
};

class Bt { // lazy: holds nothing until Open, frees all on Close
public:
    Bt();
    ~Bt();
    Bt(const Bt&) = delete;
    Bt& operator=(const Bt&) = delete;
    void Open(HWND hwnd, UINT msg); // start paired watcher, posts msg on change
    void Close();
    void Scan(bool on);
    bool Scanning() const;
    void Snap(std::vector<BtDev>& out) const;
    void Connect(const std::wstring& id, bool on);
    void Pair(const std::wstring& id);
    void Forget(const std::wstring& id);

private:
    struct Impl;
    std::shared_ptr<Impl> impl; // shared so async jobs outlive Close safely
};

}
