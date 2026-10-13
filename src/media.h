#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct Track {
    std::wstring app, title, artist, album;
    bool playing = false;
    bool canPrev = false, canNext = false, canSeek = false, canShuf = false, canRep = false;
    bool shuffle = false;
    int repeat = 0; // 0 off, 1 track, 2 list
    int64_t position = 0; // 100ns ticks
    int64_t duration = 0; // 100ns ticks, 0 = unknown
    uint64_t stamp = 0; // GetTickCount64 ms when position was valid
    double rate = 1.0;
    std::shared_ptr<const std::vector<uint8_t>> cover; // raw encoded image bytes, shared to avoid copies
    uint64_t coverId = 0; // bumps when cover bytes change

    int64_t Elapsed() const; // position extrapolated to now
    double Progress() const;
};

class Media {
public:
    Media();
    ~Media();
    Media(const Media&) = delete;
    Media& operator=(const Media&) = delete;

    void Start(HWND notify, UINT msg); // posts msg to notify on changes
    bool Snapshot(Track& track) const; // false = no active session

    void TogglePlay();
    void Next();
    void Previous();
    void Seek(double fraction); // fraction of duration, 0..1
    void ToggleShuffle();
    void CycleRepeat();
private:
    struct Impl;
    std::unique_ptr<Impl> impl; // pimpl keeps winrt headers out
};

class Radios {
public:
    Radios();
    ~Radios();
    Radios(const Radios&) = delete;
    Radios& operator=(const Radios&) = delete;
    void Start(HWND notify, UINT msg);
    bool HasWifi() const;
    bool HasBt() const;
    bool WifiOn() const;
    bool BtOn() const;
    void SetWifi(bool on);
    void SetBt(bool on);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
