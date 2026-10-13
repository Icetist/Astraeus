#pragma once
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "media.h"
#include "sys.h"
#include "bt.h"

enum : UINT { // posted to ui thread by services/worker
    WM_APP_MEDIA = WM_APP + 1,
    WM_APP_RADIOS,
    WM_APP_BRIGHT,
    WM_APP_WEATHER,
    WM_APP_WANIP,
    WM_APP_TRAY,
    WM_APP_APPBAR,
    WM_APP_STATS,
    WM_APP_BT,
};

class Hist { // fixed-size rolling graph history
public:
    explicit Hist(size_t capacity) : values(capacity, 0.0f) {} // prefilled zeros, graph starts flat
    void Push(float v) {
        values.erase(values.begin()); // drop oldest, size stays fixed
        values.push_back(v);
        ++version; // lets graphs skip redraw if unchanged
    }
    const std::vector<float>& Values() const { return values; }
    uint64_t Version() const { return version; }
    float Max() const {
        float m = 0;
        for (float v : values) m = v > m ? v : m;
        return m;
    }
    float Last() const { return values.back(); } // newest sample

private:
    std::vector<float> values;
    uint64_t version = 0;
};

class Model {
public:
    explicit Model(const Config& cfg);
    ~Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    void Start(HWND notify);
    void Tick(); // cheap per-second sample on ui thread
    void SetLive(bool active, bool processes); // gates worker polling
    void ReadAudio();
    void OnOpen(); // refresh slow stuff when drawer opens

    void OnStats(); // pull worker results under lock
    void OnMedia();
    void OnBright(WPARAM value); // -1 = no controllable display
    void OnWx();
    void OpenBt(); // starts bt watcher, only while menu shown
    void CloseBt(); // stops it and frees the list
    void OnBt();

    void SetVol(float v);
    void ToggleMute();
    void SetMic(float v);
    void ToggleMic();
    void SetBright(float v);
    void SetAwake(bool on);
    void SetRecShow(bool on);
    void CyclePower();
    void CycleOut();

    const bool portable; // laptop: power mode tile, else output tile
    int pwrMode = -1; // 0 eco, 1 balanced, 2 perf, -1 unknown
    std::vector<sys::OutDev> outs;
    std::wstring outId;
    sys::Meter meter; // peak meter for visualizer
    void StartTimer(int seconds);
    void StopTimer();
    bool TimerDone();
    double TimerLeft() const;
    double TimerFrac() const;
    int TimerMins() const { return (int)(timerTotal / 60000); }

    const Config& cfg;
    HWND notify = nullptr;

    sys::Stats stats;
    sys::Net net;
    std::vector<sys::ProcRow> procs;
    Hist cpuHist{60}, ramHist{60}, upHist{60}, downHist{60}; // 60 samples = 1 min at 1 hz

    sys::Media media;
    sys::Track track;
    bool hasTrack = false;

    sys::Radios radios;
    sys::Bt bt;
    std::vector<sys::BtDev> btDevs; // ui copy, empty while closed
    bool btOpen = false, btScan = false;
    sys::WxSvc weather;
    sys::Wx wx;

    sys::AudioDev spk = sys::AudioDev::Spk();
    sys::AudioDev mic = sys::AudioDev::Micro();
    float vol = 0, micVol = 0;
    bool muted = false, micMuted = false;

    sys::Bright bright;
    int brightPct = -1; // -1 hides brightness slider

    bool awake = false;
    bool recShow; // stay visible to screen capture, persisted
    bool timerOn = false;

private:
    void Worker();

    sys::NetSrc netSrc;
    sys::StatSrc statSrc;
    sys::ProcSrc procSrc;
    std::thread worker;
    std::mutex lock; // guards fields shared with worker
    std::condition_variable wake;
    bool quit = false, pollOn = false, wantProcs = false, ready = false; // ready = fresh sample waiting
    sys::Stats newStats; // worker writes, OnStats copies out
    std::vector<sys::ProcRow> newProcs;
    ULONGLONG timerEnd = 0, timerTotal = 0;
    bool timerDone = false; // one-shot flag, cleared on read
    unsigned long long lastIdle = 0, lastTotal = 0; // prev cpu ticks for delta
};
