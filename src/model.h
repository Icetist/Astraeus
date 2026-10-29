#pragma once
#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "lyrics.h"
#include "media.h"
#include "spectrum.h"
#include "sys.h"
#include "bt.h"
#include "wifi.h"
#include "mixer.h"
#include "share.h"
#include "notify.h"
#include "calendar.h"
#include "shelf.h"
#include "battery.h"
#include "accent.h"


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
    WM_APP_PICKED, // the color picker finished: wParam rgb, lParam 1 picked / 0 cancelled
    WM_APP_CAL, // calendar events fetched
    WM_APP_SHELF, // a shelf preview is ready
    WM_APP_ACCENT, // windows' accent color changed
    WM_APP_HUDVOL, // the speakers' level or mute changed: wparam level 0..1000, lparam muted
    WM_APP_HUDBRIGHT, // the panel's brightness changed: wparam percent
    WM_APP_VOLKEY, // a volume key, taken from windows: wparam the key
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
    void SetLive(bool active, bool processes, bool ping = false); // gates worker polling
    void ReadAudio();
    void OnOpen(); // refresh slow stuff when drawer opens
    void Prefetch(); // a few seconds after launch: weather ready before the panel first opens

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
    void SetLook(int look); // appearance: 0 dark, 1 light, 2 glass, 3 custom, 4 halobar's; persisted
    void SetPref(int& field, const wchar_t* key, int value); // store a settings value and remember it
    void OpenSettings(); // astraeus settings sheet, one sheet at a time
    void PickScreen(const std::wstring& name); // live on this screen only
    void CloseSettings();
    void OpenCal(); // the calendar sheet, from the weather card
    void CloseCal();
    void OpenMixer(); // per-app volume, from the volume slider on Home
    void CloseMixer();
    void SetAccent(int mode, int hex); // persisted, drawer crossfades to it; mode 0 wallpaper, 1 custom, 2 windows
    void SetNotifyPills(bool on); // persisted, starts or stops the watcher
    void SetQuiet(bool on); // do not disturb, shared with halobar
    bool PollShared(); // once a second: do not disturb, halobar's pills and theme; true when the look changed
    bool PillsWanted() const { return !quiet && (notifyPills || haloPills); } // windows notifications drop as pills now
    bool Following() const { return look == kLookHalo; } // the look is halobar's
    bool HaloRuns() const { return haloRuns; }
    int Look() const; // what's drawn: halobar's material while following it, else the pick
    int AccentMode() const; // these four: halobar's while following it and it has its own, else ours
    int AccentHex() const;
    int CustomBg() const;
    int GlassBlur() const;
    int BgOpacity() const; // the solid looks' body: halobar's while following it, the default while it follows us back
    bool CommitHex(); // apply the typed hex if it parses, ends typing either way
    static bool ParseHex(const std::wstring& text, int& rgb); // #rgb, #rrggbb, with or without #
    void CyclePower();
    void CycleOut();

    const bool portable; // laptop: power mode tile, else output tile
    int pwrMode = -1; // 0 eco, 1 balanced, 2 perf, -1 unknown
    int batPct = -1; // laptop battery, -1 none or unknown
    bool charging = false;
    std::vector<sys::OutDev> outs;
    std::wstring outId;
    sys::Meter meter; // peak meter for visualizer
    std::unique_ptr<sys::Spectrum> spectrum; // frequency bands, only while a visualizer is on screen
    ULONGLONG spectrumAsked = 0;
    void Bands(float (&out)[sys::Spectrum::kBands]); // starts the analyser on first use
    void IdleSpectrum(); // stops it once nothing has asked for a few seconds
    float pingMs = -2, jitterMs = 0; // latency to the internet while the network page shows; -1 no answer, -2 not measured
    Hist pingHist{60};
    // stopwatch and focus sessions, on the timer's own once-a-second tick
    int toolTab = 0; // 0 timer, 1 stopwatch, 2 focus
    bool swOn = false;
    ULONGLONG swStart = 0, swAccum = 0; // running since, and time banked before it
    std::vector<ULONGLONG> laps;
    ULONGLONG SwMs() const { return swAccum + (swOn ? GetTickCount64() - swStart : 0); }
    void SwToggle();
    void SwReset();
    void SwLap();
    bool focusOn = false, focusBreak = false;
    int focusRound = 1, focusMin = 25, breakMin = 5;
    ULONGLONG focusEnd = 0, focusLen = 0;
    int focusEvent = 0; // 1 a focus block ended, 2 a break ended; main shows it
    void StartFocus(int focus, int rest);
    void StopFocus();
    void SkipFocus(); // to the next block now
    double FocusLeft() const;
    double FocusFrac() const;
    void StartPicker(); // the screen color picker
    bool pickPending = false; // asked for: starts once the panel has gone up, so it's not in the way
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
    sys::LyricSrc lyrics; // synced lyrics, looked up only while the fullscreen player wants them

    sys::Radios radios;
    sys::Bt bt;
    std::vector<sys::BtDev> btDevs; // ui copy, empty while closed
    bool btOpen = false, btScan = false;
    sys::Share share; // plays the main output on extra devices, idle unless something is shared
    std::vector<sys::ShareOut> shareOuts; // ui copy for the shared-audio tab
    bool shareTab = false; // bluetooth sheet shows the shared-audio tab
    sys::Wifi wifi;
    sys::Mixer mixer; // holds nothing unless the mixer sheet is open
    bool mixerOpen = false;
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
    int look = 0; // appearance picked: 0 dark (default), 1 light, 2 glass, 3 custom, 4 follow halobar, 5 starfall; Look() is what's drawn
    static constexpr int kLookHalo = 4;
    static constexpr int kLookStarfall = 5; // ours alone: halobar wears it only by following us, and never hands it back
    struct Halo { // halobar's own theme, read only while following it
        int look = -1; // -1 it follows us back, else 0..3 like ours
        int accent = -1; // -1 ours, 0 wallpaper, 1 the hex
        int accentHex = 0x5B8CFF, customBg = 0x121826, glassBlur = 100, opacity = 100; // halobar's defaults
        bool here = false; // its settings key exists: installed, and run at least once
        bool FollowsUs() const { return here && look < 0; } // then following it back would only loop to dark: settings hides the choice
        bool operator==(const Halo&) const = default;
    } halo;
    int customBg = 0x121826; // custom look: the panel background, 0xRRGGBB
    int lyricOffsetMs = 0; // fullscreen lyrics nudged later (+) or earlier (-), [ and ] change it
    int glassBlur = 100; // glass: 0 clear .. 100 fully frosted
    int edge = 0; // 0 top (default), 1 left, 2 right: the user's pick
    int haloEdge = -1; // the edge halobar's dock holds, in our numbering; -1 none (bottom, or not running)
    int Edge() const { return edge != haloEdge ? edge : edge == 0 ? 1 : 0; } // where the panel really docks: never on halobar's edge, off a side to the top, off the top to the left
    int screens = 0; // 0 one screen, 1 every screen (the panel follows the mouse)
    int islandSecs = 0; // the pill over maximized windows, after the mouse leaves: 0 brief, up to kIslandMaxSecs, -1 always
    static constexpr int kIslandMaxSecs = 300;
    bool IslandAlways() const { return islandSecs < 0; }
    UINT IslandLingerMs(UINT brief) const { return islandSecs > 0 ? (UINT)islandSecs * 1000 : brief; } // once the mouse is gone
    std::wstring screen; // the one screen, by device name; empty = primary
    std::vector<sys::Screen> screenList; // connected screens, read when settings opens
    int accentMode = 0; // 0 colors from the wallpaper (default), 1 custom
    int accentHex = 0xB8C4FF; // custom accent, 0xRRGGBB
    bool hexFocus = false; // the settings hex field is taking keystrokes
    bool passFocus = false, passShow = false, passWrong = false; // the wi-fi password field: taking keystrokes, shown in clear, last try failed
    std::string passFor; // raw ssid of the network being joined
    std::wstring passEdit; // what's typed so far, wiped once used
    bool Typing() const { return hexFocus || passFocus || cityFocus; } // the panel holds the keyboard
    void AskPass(const sys::WifiNet& n); // a new secured network: type its password in the panel
    void SubmitPass();
    void CancelPass();
    int hexTarget = 0; // what the typed hex sets: 0 the accent, 1 the custom background
    std::wstring hexEdit; // what's typed so far, without the #
    int notifyPills = 0; // 1: windows notifications also drop as a pill
    int quiet = 0; // do not disturb: no notification pills; shared with halobar through the same value
    bool haloPills = false; // halobar runs with its pills on: it stays quiet while we're up, so we show them
    int fullLyrics = 1; // the fullscreen player shows synced lyrics beside the cover (on by default)
    bool settingsOpen = false;
    bool calOpen = false, calFresh = false; // fresh: just opened, start on this month
    bool calForecast = false; // the calendar sheet shows the forecast instead of the month
    sys::cal::Calendars cals; // the user's own calendars from their private links, shared with halobar
    bool calAgenda = false, calLinks = false; // the sheet shows the picked day's events / the connected calendars, in the month's place
    bool calWaiting = false; // "connect" was pressed: the next calendar link copied anywhere is taken
    void StartCalWait(bool google);
    void StopCalWait();
    bool OnClipboard(); // true when a calendar link was taken
    static constexpr UINT_PTR kCalWaitTimer = 7; // the drawer's timer that ends the wait
    int reduceMotion = 0; // calmer animation: no bounce, no drifting, no sliding
    sys::NotifyWatch notifyWatch; // only runs while PillsWanted
    bool timerOn = false;
    sys::Shelf shelf; // files and notes dropped on the island
    bool shelfHot = false; // something droppable is over the panel right now
    uint64_t shelfDrag = 0; // a shelf item the page wants dragged out, started after the frame
    int powerPills = 1; // a pill when it's plugged in or unplugged
    int batLow = 20; // the low battery pill shows at this percent, 5..50
    bool lowShown = false; // shown once per discharge
    void ReadBattery(); // on power events only, nothing polls it
    sys::BatteryInfo batInfo; // the battery tile's details, read when its tip opens
    int activityBubble = 1; // a running timer, stopwatch or focus session sits in a bubble beside the pill
    enum Act { kActNone, kActTimer, kActWatch, kActFocus };
    int Activity() const; // which one runs, the timer first
    std::wstring ActivityText() const; // its time, short enough for the bubble
    float ActivityFrac() const; // its ring, 0..1
    int rememberPage = 1, defaultPage = 0, lastPage = 0; // the page the panel opens on, by identity
    enum PageId { kPgHome, kPgMedia, kPgSystem, kPgNetwork, kPgTools, kPgShelf, kPageIds }; // what a page is, wherever it sits on the rail
    std::vector<int> pages; // the rail's pages top to bottom, never empty
    int SlotOf(int id) const; // its place on the rail, -1 hidden
    bool HasPage(int id) const { return SlotOf(id) >= 0; }
    int PageAt(int slot) const { return pages[(size_t)(std::clamp)(slot, 0, (int)pages.size() - 1)]; } // past either end: the end
    int OpenPage() const; // the last or the picked page, or the first shown when that one is hidden
    void SetPages(const std::vector<int>& order); // persisted by name; an empty list is refused
    static const wchar_t* PageName(int id);
    static int PageGlyph(int id);
    enum ToolTile { kTlSnip, kTlColor, kTlAwake, kTlAlerts, kTlDnd, kTlSaver, kTlAir, kTlSettings, kToolTiles }; // the tools page's quick tiles
    std::vector<int> tiles; // the ones shown, in order, may be empty
    bool TileHere(int id) const { return id != kTlAwake || portable; } // keeping the screen awake is for laptops
    void SetTiles(const std::vector<int>& order);
    bool tilesEdit = false; // the tools page shows its tiles with x and + marks; ends when the panel closes
    static const wchar_t* TileName(int id);
    static int TileGlyph(int id);
    int stageHide = 1; // the fullscreen player's controls fade while the mouse rests (on by default)
    int uiScale = 100; // percent, 80..130, folded into the dpi scale
    int lyricScale = 100; // percent, the fullscreen player's lyric type; 100 is the size it always had
    static constexpr int kLyricMin = 80, kLyricMax = 150;
    int bgOpacity = 100; // percent, the panel and pill body on the solid looks; BgOpacity() is what's drawn
    static constexpr int kMinOpacity = 40, kMaxOpacity = 100; // below 40 the cards float on nothing; halobar keeps the same range
    static constexpr int kAccentWindows = 2; // accent source: windows' own accent color
    sys::WinAccent winAccent; // listened to only while that source is picked
    int winAccentHex = -1;
    bool cityFocus = false; // the weather city field is taking keystrokes
    std::wstring cityEdit; // what's typed so far
    void CommitCity(); // empty goes back to finding you by ip
    int pillAlways = 0; // the resting pill stays up with the clock
    int pillDesk = 0; // the pill stays up while the desktop is what's under it, always or not
    enum Widget { kWNone, kWWeather, kWBattery, kWCpu, kWRam, kWNet, kWidgets };
    int pillLeft = kWWeather, pillRight = kWCpu; // its two status widgets
    std::wstring PillWidget(int which, int& icon) const; // what a widget shows now, empty when it has nothing
    int mediaCompact = 0, pillMusic = 1, showArt = 1, vizOn = 1; // media modules
    int hud = 0; // astraeus' own volume and brightness pill, experimental, off by default

private:
    void Worker();

    sys::NetSrc netSrc;
    sys::StatSrc statSrc;
    sys::ProcSrc procSrc;
    std::thread worker;
    std::mutex lock; // guards fields shared with worker
    std::condition_variable wake;
    bool quit = false, pollOn = false, wantProcs = false, ready = false; // ready = fresh sample waiting
    bool wantPing = false; // the network page is showing: measure latency
    float newPing = -2; // worker writes, -1 no answer, -2 not measured
    HANDLE icmp = INVALID_HANDLE_VALUE; // opened the first time latency is wanted
    sys::Stats newStats; // worker writes, OnStats copies out
    std::vector<sys::ProcRow> newProcs;
    ULONGLONG timerEnd = 0, timerTotal = 0;
    bool timerDone = false; // one-shot flag, cleared on read
    unsigned long long lastIdle = 0, lastTotal = 0; // prev cpu ticks for delta
    bool shown = false; // panel open, ui thread copy of SetLive
    int unsampled = 0; // seconds since the last stats sample
    void Sample(int seconds); // read cpu/ram/net, push one graph point per second covered
    void SyncNotify(); // watcher on exactly while PillsWanted
    bool haloRuns = false; // a halobar window exists
    bool watching = false; // the watcher was last asked to run
};
