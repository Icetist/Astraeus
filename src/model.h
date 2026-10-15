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
#include "wifi.h"
#include "share.h"
#include "notify.h"

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
    WM_APP_UPDATE,
    WM_APP_WIFI,
    WM_APP_SHARE,
    WM_APP_NOTIFY,
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
    void OpenWifi(); // wi-fi picker sheet: opens wlan, only while shown
    void CloseWifi(); // frees it and the list
    void OnWifi();
    void ReopenWifi(); // start over, e.g. adapter appeared after opening
    void DropWifiList(); // free the list once the sheet has faded out
    void OnShare(); // refresh the shared-audio list, only while its tab shows

    void SetVol(float v);
    void ToggleMute();
    void SetMic(float v);
    void ToggleMic();
    void SetBright(float v);
    void SetAwake(bool on);
    void SetLook(int look); // appearance: 0 dark, 1 light, 2 glass; persisted
    void SetPref(int& field, const wchar_t* key, int value); // store a settings value and remember it
    void OpenSettings(); // astraeus settings sheet, one sheet at a time
    void PickScreen(const std::wstring& name); // live on this screen only
    void CloseSettings();
    void SetAccent(int mode, int hex); // persisted, drawer crossfades to it
    void SetNotifyPills(bool on); // persisted, starts or stops the watcher
    bool CommitHex(); // apply the typed hex if it parses, ends typing either way
    static bool ParseHex(const std::wstring& text, int& rgb); // #rgb, #rrggbb, with or without #
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
    sys::Share share; // plays the main output on extra devices, idle unless something is shared
    std::vector<sys::ShareOut> shareOuts; // ui copy for the shared-audio tab
    bool shareTab = false; // bluetooth sheet shows the shared-audio tab
    sys::Wifi wifi;
    std::vector<sys::WifiNet> wifiNets; // ui copy, empty while closed
    bool wifiOpen = false, wifiScan = false;
    sys::WifiState wifiState = sys::WifiState::Ok;
    sys::WxSvc weather;
    sys::Wx wx;

    sys::AudioDev spk = sys::AudioDev::Spk();
    sys::AudioDev mic = sys::AudioDev::Micro();
    float vol = 0, micVol = 0;
    bool muted = false, micMuted = false;

    sys::Bright bright;
    int brightPct = -1; // -1 hides brightness slider

    bool awake = false;
    int look = 0; // appearance: 0 dark (default), 1 light, 2 glass
    int glassBlur = 100; // glass: 0 clear .. 100 fully frosted
    int edge = 0; // 0 top (default), 1 left, 2 right
    int screens = 0; // 0 one screen, 1 every screen (the panel follows the mouse)
    std::wstring screen; // the one screen, by device name; empty = primary
    std::vector<sys::Screen> screenList; // connected screens, read when settings opens
    int accentMode = 0; // 0 colors from the wallpaper (default), 1 custom
    int accentHex = 0xB8C4FF; // custom accent, 0xRRGGBB
    bool hexFocus = false; // the settings hex field is taking keystrokes
    std::wstring hexEdit; // what's typed so far, without the #
    int notifyPills = 0; // 1: windows notifications also drop as a pill
    bool settingsOpen = false;
    sys::NotifyWatch notifyWatch; // only runs while notification pills are on
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
    bool shown = false; // panel open, ui thread copy of SetLive
    int unsampled = 0; // seconds since the last stats sample
    void Sample(int seconds); // read cpu/ram/net, push one graph point per second covered
};
