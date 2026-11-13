#pragma once
#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <map>
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
#include "desk.h"
#include "extras.h"
#include "workspaces.h"
#include "mixer.h"
#include "share.h"
#include "notify.h"
#include "calendar.h"
#include "shelf.h"
#include "battery.h"
#include "accent.h"
#include "alarms.h"


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
    WM_APP_WS, // the workspaces changed
    WM_APP_UPDATES, // the update count is in
    WM_APP_PING, // a host answered, or didn't
    WM_APP_FEEDS, // a widget's feed has something new
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
    void SetArtBack(bool on); // the media page's album art background, shared with halobar too
    bool PollShared(); // once a second: do not disturb, halobar's pills and theme; true when the look changed
    bool PillsWanted() const { return !quiet && (notifyPills || haloPills); } // windows notifications drop as pills now
    bool Following() const { return look == kLookHalo; } // the look is halobar's
    bool WatchesDesk() const { return pillDesk != 0 || deskPill != 0 || deskApps != 0 || appsTheme != 0; } // something here depends on what's under the panel
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
    void SetOutTo(const std::wstring& id); // that output becomes the default: every app following the default goes with it
    std::vector<sys::MediaSource> mediaSources; // every app reporting media, as of the last change
    std::wstring mediaPin; // the one followed by hand, empty: whatever's playing

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
    int toolTab = 0; // 0 timer, 1 stopwatch, 2 focus, 3 alarm, 4 to-do, 5 servers
    enum ToolTab { kTabTimer, kTabWatch, kTabFocus, kTabAlarm, kTabTodo, kTabHosts, kToolTabs };
    struct Todo {
        std::wstring text;
        bool done = false;
        int due = 0; // yyyymmdd, 0 none; shown on the calendar's day too
    };
    static int TodoToday(); // today as yyyymmdd
    static int TodoAddDays(int ymd, int days);
    bool TodoDueOn(int y, int mo, int d) const; // anything not done due that day: the calendar marks it
    std::vector<Todo> todos; // the tools page's to-do list, kept in the registry
    std::vector<std::wstring> hosts; // the servers it watches
    enum Field { kFieldNone, kFieldTodo, kFieldHost, kFieldTimer, kFieldAlarmLabel };
    int field = kFieldNone; // the list field taking keystrokes
    std::wstring fieldEdit;
    static constexpr size_t kFieldMax = 120;
    static constexpr int kMaxTodos = 50, kMaxHosts = 12;
    void CommitField(); // enter: the typed line joins its list
    void SaveTodos();
    void SaveHosts(); // and the monitor restarts with them
    AlarmStore alarms;
    int alarmBellTick = 0, alarmSeenTick = 0;
    int alarmEditId = -1; // -1 list view, 0 creating new alarm, >0 editing existing alarm
    int alarmHour = 8, alarmMin = 0;
    bool alarmDaily = false;
    std::wstring alarmLabel;
    void SaveAlarms();
    void SaveAlarmSeen();
    void BeginAlarmEdit(int id);
    void SaveAlarmEditor();
    void CancelAlarmEditor();
    bool TickAlarms();
    bool SnoozeAlarm();
    bool DismissAlarm();
    sys::Workspaces ws; // a tiling manager's workspaces or windows' desktops, only while a widget or tile shows them
    sys::WsState wsNow;
    sys::extras::Updates updates;
    sys::extras::UpdateCount updateNow;
    sys::extras::Pinger pinger;
    std::vector<sys::extras::HostHealth> hostNow;
    void SyncExtras(); // once a second: the workspaces and the monitor run only while something shows them
    bool WantsWs() const;
    int updateTick = 0;
    sys::Tiler tiler = sys::Tiler::kNone; // found at start: its workspaces tile turns on by itself
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
    std::wstring TimerName() const; // its length, for the notification: "25 min" or "1:30"

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
    sys::Mixer mixer; // holds nothing unless the mixer sheet is open or a mixer group needs it
    bool mixerOpen = false;
    sys::Desk desk; // the mixer's groups, chat mix, ducking and profiles, shared with halobar
    sys::DeskEngine deskEngine; // applies them, in whichever app claimed it first
    std::vector<sys::OutDev> mics; // input choices, read while the mixer shows
    std::wstring micId;
    int deskEdit = -1; // the group whose editor is open, -1 none
    int nameFor = -1; // a group's name is being typed
    std::wstring nameEdit;
    std::wstring deskSay; // what a mixer shortcut did, for the banner; taken once
    static constexpr int kMixKeys = 12; // mic, next profile, chat mix both ways, eight groups
    static constexpr size_t kNameMax = 24;
    void DeskTick(float dt, bool fast); // once a second, and several times a second while ducking or metering
    bool DeskFast() const { return mixerOpen || deskEngine.NeedsMeter(desk); }
    void DeskSave(); // an edit: stored for halobar and applied at once
    void DeskKey(int k); // a mixer shortcut
    void CommitName();
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
    int look = 0; // appearance picked: 0 dark (default), 1 light, 2 glass, 3 custom, 4 follow halobar, 5 starfall, 6 clear, 7 chrome; Look() is what's drawn
    static constexpr int kLookHalo = 4;
    static constexpr int kLookStarfall = 5; // ours alone: halobar wears it only by following us, and never hands it back
    static constexpr int kLookClear = 6; // picked under glass: the resting pill is only ink, the panel glass on the wallpaper's side
    static constexpr int kLookChrome = 7; // polished metal; halobar numbers it the same, so following works both ways
    struct Halo { // halobar's own theme, read only while following it
        int look = -1; // -1 it follows us back, else 0..3 or clear, like ours
        int glassTint = -1;
        int accent = -1; // -1 ours, 0 wallpaper, 1 the hex
        int accentHex = 0x5B8CFF, customBg = 0x121826, glassBlur = 100, opacity = 100; // halobar's defaults
        bool here = false; // its settings key exists: installed, and run at least once
        bool FollowsUs() const { return here && look < 0; } // then following it back would only loop to dark: settings hides the choice
        bool operator==(const Halo&) const = default;
    } halo;
    int customBg = 0x121826; // custom look: the panel background, 0xRRGGBB
    int lyricOffsetMs = 0; // fullscreen lyrics nudged later (+) or earlier (-), [ and ] change it
    int glassBlur = 100; // glass: 0 clear .. 100 fully frosted
    struct ThemeSet { int look, accentMode, accentHex, customBg, glassBlur, bgOpacity, glassTint; };
    int appsTheme = 0; // a whole theme of its own while an app is under the panel
    ThemeSet otherSet{}; // the theme not in the live fields: every reader keeps reading those, so nothing can be missed
    int setNow = 0; // which is in them: 0 the desktop's, 1 over apps
    int setTab = -1; // the one settings is editing, -1 whichever applies
    int haloSetWas = 0; // which of halobar's two themes was last read
    bool overApp = false; // an app is under it right now
    void SyncThemeSet(); // the theme that should be live now
    std::wstring ThemeKey(const wchar_t* key) const; // where this value is saved, for the theme that's live
    int glassTint = -1; // glass: its tint, 0xRRGGBB; -1 the plain dark one
    int GlassTint() const { return Following() ? halo.glassTint : glassTint; }
    bool Glassy() const { int l = Look(); return l == 2 || l == kLookClear; } // the look draws on the live blur
    int edge = 0; // 0 top (default), 1 left, 2 right: the user's pick
    int haloEdge = -1; // the edge halobar's dock holds, in our numbering; -1 none (bottom, or not running)
    int haloReach = 0; // pixels its bottom dock rises from the screen's bottom, 0 when it isn't there: the sidebar ends above it
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
    bool Typing() const { return hexFocus || passFocus || cityFocus || faceKeys || faceSearch || nameFor >= 0 || field != kFieldNone || shelfField != kShelfNone; } // the panel holds the keyboard
    void AskPass(const sys::WifiNet& n); // a new secured network: type its password in the panel
    void SubmitPass();
    void CancelPass();
    int hexTarget = 0; // what the typed hex sets: 0 the accent, 1 the custom background
    std::wstring hexEdit; // what's typed so far, without the #
    int notifyPills = 0; // 1: windows notifications also drop as a pill
    int quiet = 0; // do not disturb: no notification pills; shared with halobar through the same value
    bool haloPills = false; // halobar runs with its pills on: it stays quiet while we're up, so we show them
    int fullLyrics = 1; // the fullscreen player shows synced lyrics beside the cover (on by default)
    int mediaLyrics = 0; // the media page shows them too, beside the song (off by default)
    int fahr = 0; // temperatures in fahrenheit; until it's set in settings, as the ini says
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
    sys::Shelf shelf; // what was dropped on the island, and everything copied while that's switched on
    bool shelfHot = false; // something droppable is over the panel right now
    uint64_t shelfDrag = 0; // a shelf item the page wants dragged out, started after the frame
    uint64_t shelfPaste = 0; // a card the page wants pasted into the app in front, also after the frame
    int shelfPasteHow = 0; // a sys::Shelf::CopyHow
    std::vector<size_t> shelfPicks; // the files of a stack that drag or paste is about; empty: all of them
    enum ShelfField { kShelfNone, kShelfSearch, kShelfNote };
    int shelfField = kShelfNone; // the shelf's field holding the keyboard
    std::wstring shelfQuery, shelfEdit; // the search, and a note being typed
    uint64_t shelfNoteFor = 0;
    void CommitShelfNote(); // the typed note onto its card, ends typing
    int shelfCapture = 0; // everything copied goes to the shelf too: off unless asked for
    int shelfHistory = sys::Shelf::kHistDef, shelfExpire = sys::Shelf::kExpireNever, shelfClearStart = 0;
    void SyncShelf(); // hands those four to the shelf
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
    enum PageId { kPgHome, kPgMedia, kPgSystem, kPgNetwork, kPgTools, kPgShelf, kPgWidgets, kPageIds }; // what a page is, wherever it sits on the rail
    std::vector<int> faceOn; // the widgets on the widgets page, in order
    std::map<int, int> faceSize; // each one's size there if it was ever chosen: 0 small, 1 wide, 2 large
    void SetFaceSize(int kind, int size);
    void PlaceFaces(const std::vector<int>& order); // the same widgets, in a new order
    void SetFaces(const std::vector<int>& on, const std::map<int, int>& sizes);
    bool faceSearch = false; // the widget gallery's search is taking the keyboard
    std::wstring faceQuery; // and what's typed there
    static constexpr size_t kFaceQueryMax = 40;
    int pageVisit = 0; // bumps as the page changes or the panel closes: what a page had open over itself goes then. not a timer: an idle panel draws no frames
    int facePanel = -1; // the one open in full
    bool faceKeys = false; // and it's taking the keyboard
    bool facesDue = false; // the widgets page drew: its clocks want the next second too
    void ToggleFace(int kind);
    void OpenFace(int kind);
    void CloseFace();
    void SyncFaces(); // what they fetch: everything while the panel's up, screen time's quiet note always
    std::vector<int> pages; // the rail's pages top to bottom, never empty
    int SlotOf(int id) const; // its place on the rail, -1 hidden
    bool HasPage(int id) const { return SlotOf(id) >= 0; }
    int PageAt(int slot) const { return pages[(size_t)(std::clamp)(slot, 0, (int)pages.size() - 1)]; } // past either end: the end
    int OpenPage() const; // the last or the picked page, or the first shown when that one is hidden
    void SetPages(const std::vector<int>& order); // persisted by name; an empty list is refused
    static std::vector<int> DefaultPages();
    static std::vector<int> DefaultTiles();
    static const wchar_t* PageName(int id);
    static const wchar_t* PageAbout(int id);
    static int PageGlyph(int id);
    enum ToolTile { kTlSnip, kTlColor, kTlAwake, kTlAlerts, kTlDnd, kTlSaver, kTlAir, kTlSettings, kTlWork, kTlLang, kTlBin, kTlUpdates, kToolTiles }; // new ones go last: saved lists keep their meaning // the tools page's quick tiles
    std::vector<int> tiles; // the ones shown, in order, may be empty
    bool TileHere(int id) const { return id == kTlWork ? tiler != sys::Tiler::kNone : id != kTlAwake || portable; } // keeping the screen awake is for laptops, workspaces for a pc with a tiling manager
    void CheckTiler(); // installed or removed since the last look: the tile, pill word and dashboard tile come and go with it
    ULONGLONG tilerAt = 0;
    void SetTiles(const std::vector<int>& order);
    static const wchar_t* TileName(int id);
    static const wchar_t* TileAbout(int id);
    static int TileGlyph(int id);
    int stageHide = 1; // the fullscreen player's controls fade while the mouse rests (on by default)
    int stageThump = 1; // the fullscreen cover thumps with the beat
    int stageLyrBtn = 1, stageEditBtn = 1; // the fullscreen player's corner: wrong lyrics, and its editor
    int uiScale = 100; // percent, 80..130, folded into the dpi scale
    enum { kContrastAuto, kContrastOn, kContrastOff };
    static constexpr int kTextMin = 100, kTextMax = 200;
    int a11yContrast = kContrastAuto; // high contrast: as windows has it, or always, or never
    int a11yFollow = 1; // windows' "make text bigger" counts
    int a11yText = 100; // percent, the text size picked here
    int a11yKeys = 1; // tab, enter and the arrows
    int a11yReader = 1; // screen readers can see the panel
    void ReadAccess(); // what windows has for high contrast and text size: at start, and when its settings change
    bool HighContrast() const;
    int TextPct() const; // the larger of the size picked here and windows'
    int ZoomPct() const { return uiScale * TextPct() / 100; } // the size everything's drawn at
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
    enum Widget { kWNone, kWWeather, kWBattery, kWCpu, kWRam, kWNet, kWWork, kWLang, kWTitle, kWDisk, kWGpu, kWMarket, kWidgets }; // new ones go last: their numbers are saved
    static constexpr char kMarketQuote[] = "quote:^GSPC"; // the pills' market: the s&p 500
    int pillFull = 0; // it stays up over fullscreen apps, rather than going away for them
    int pillFullOpen = 1; // and a click on it there opens the panel; off: only ever the pill
    bool pillGpu = false, pillMeters = false; // a showing pill has a gpu reading on it; a cpu or ram one
    void SetPillMeters(bool gpu, bool meters); // what a showing pill wants kept fresh
    int pillLeft = kWWeather, pillRight = kWCpu; // its two status widgets
    int deskPill = 0; // the long pill on the desktop, in its own window
    bool LongPill() const { return deskPill != 0 && pillDesk != 0; } // the desktop pill's long form: with no pill on the desktop, no long one either
    bool AnyLong() const { return LongPill() || deskApps != 0; } // the long pill shows somewhere: on the desktop, over apps, or both
    enum { kEditFromLong, kEditFromDesk, kEditFromApps }; // who opened the long pill's editor: itself or settings, or the regular pill on the desktop or over an app
    int deskFillApps = 0; // its body while an app is under it
    int deskLenApps = 560, deskGapApps = 10, deskFloatApps = 1, deskThickApps = 30; // not on desktop it can be its own length, stuck or floating, thickness
    int deskPreview = -1; // its editor is showing this one's theme: 0 desktop, 1 not on desktop; -1 not open
    enum StudioAsk { kStudioNone, kStudioWidgets, kStudioTiles, kStudioPages };
    int studioAsk = kStudioNone; // an editor over the wallpaper, asked for: it opens after the frame
    int tourNext = 1; // the tour plays when the panel first opens after a launch; everyone gets it once
    bool tourAsk = false; // settings asked for it: it starts after the frame
    int studioKind = kStudioWidgets; // which one it is
    bool studioToSettings = false; // it was left for settings: they open with the panel
    bool railAcross = false; // the rail as the panel has it: along the top or down the side
    float railLen = 0, railThick = 0;
    bool studioOpen = false; // the widgets' one is open: what's in it keeps fetching
    int studioRun = 0; // bumps as an editor over the wallpaper opens: its undo starts afresh
    float faceAreaW = 0, faceAreaH = 0, tileAreaW = 0, tileAreaH = 0; // the room they have on their pages: their editors show them at just this size
    int deskEdge = 0, deskPos = 500, deskLen = 560, deskThick = 30, deskGap = 10, deskFloat = 1, deskFill = 0; // its edge, place along it in thousandths, size, gap, stuck or floating, body
    int deskApps = 0; // it also stands in for the small pill over maximized apps, on the same rules
    enum DeskItem { kDClock = 20, kDDate, kDVolume, kDWifi, kDMusic, kDTimer, kDCover, kDBars, kDScore, kDLyrics }; // beyond the widgets above; new ones go last
    bool PillLyrics() const; // the long pill shows the sung line somewhere: the song's lyrics are looked up for it
    double LyricWait() const; // seconds until its line changes; -1: nothing coming while it plays
    int scoreAt = 0; // the long pill's score: which of the followed leagues' games it shows; the wheel over it steps on
    struct ScoreSeen { bool live = false; double end = 0; }; // a game seen on, and when it was seen to finish
    mutable std::map<std::wstring, ScoreSeen> scoreSeen; // by league and game: a game just over stays on the pill a while
    mutable std::mutex scoreLock; // the pills ask from more than one window
    void StepScore(int by) { scoreAt += by; }
    std::vector<int> deskItems; // what it shows, in order; the clock anchors the middle
    bool deskEditAsk = false; // settings asked for the editor: it opens after the frame
    static std::vector<int> DeskDefaults();
    static const wchar_t* DeskItemName(int kind);
    std::wstring DeskItemText(int kind, int& icon) const; // what an item shows now; empty and no icon: nothing right now
    void SetDeskItems(const std::vector<int>& items);
    std::vector<int> deskItemsApps; // its own set when not on desktop, while deskSplit is on
    int deskSplit = 0; // off: the same widgets everywhere
    static constexpr int kMidAuto = -1, kMidNone = -2; // what holds the long pill's middle: the clock if it's there, or nothing (one centred row); else an item's kind
    int deskMid = kMidAuto, deskMidApps = kMidAuto; // for the desktop's set, and for the set when not on desktop
    void SetDeskItemsApps(const std::vector<int>& items);
    void WantDeskAudio(); // the speakers read, for its volume
    std::wstring PillWidget(int which, int& icon) const; // what a widget shows now, empty when it has nothing
    int mediaCompact = 0, pillMusic = 1, showArt = 1, vizOn = 1; // media modules
    int artBack = 0; // the media page sits on its cover, blurred across the whole panel
    int stageArt = 0; // the fullscreen player on its cover too, switched apart from the media page
    static constexpr int kVizAlphaMin = 10, kVizGainMin = 50, kVizGainMax = 200, kSlideStep = 5; // percent ranges of the two shared sliders
    int vizAlpha = 60; // percent: how opaque the visualizer sits over the album art, shared with halobar
    int vizGain = 100; // percent: how hard the visualizer answers the music, shared with halobar
    float VizGain() const { return vizGain / 100.0f; }
    bool Levels(float& left, float& right); // the meter's peaks, scaled by the sensitivity
    int keyOnly = 0; // only the shortcut opens the closed island: hover and clicks on it do nothing
    int appKeyOnly = 0; // an app window under the edge: hovering opens nothing, only the shortcut does
    static constexpr int kAppHoverMs = 500, kAppHoverMax = 1000, kAppHoverStep = 50; // the dwell over an app window: default, most, slider step
    int appHoverMs = kAppHoverMs; // how long the mouse rests at the edge over an app before the pill drops
    int hud = 0; // astraeus' own volume and brightness pill, experimental, off by default
    int vizWave = 0; // the media page and fullscreen draw a waveform across, behind the art, instead of the soft glow
    int vizFull = 0; // the visualizer ignores how loud windows plays: quiet music still moves it

    enum Layout { kLayHorizontal, kLayVertical, kLayouts }; // the panel's shape: the wide island, or a narrow column
    int layout = kLayHorizontal;
    int pinned = 0; // the panel stays open, windows make room for it
    int floating = 0; // the panel is a card clear of the edge, not dripping from it
    static const wchar_t* LayoutName(int l);

    struct BoardTile { // one tile of the display dashboard, in grid cells
        int kind = 0, x = 0, y = 0, w = 2, h = 2;
    };
    enum BoardKind { kBdClock, kBdCalendar, kBdWeather, kBdMedia, kBdLyrics, kBdCpu, kBdGpu, kBdRam, kBdNet, kBdDisk, kBdProcs, kBdVolume, kBdTodo, kBdWork, kBdShelf, kBdTimer, kBdServers, kBdBattery, kBoardKinds }; // new ones go last
    bool BoardOffered(int kind) const; // whether this pc can show it: no workspaces without a tiling manager, no battery on a desktop
    static constexpr int kBoardCols = 12; // the grid is always 12 across; its rows follow the screen's shape
    struct BoardSpot { // a screen that shows the dashboard, with its own tiles
        std::wstring screen; // the monitor's own id, the same after a replug
        std::vector<BoardTile> tiles;
        bool edited = false; // never arranged: the preset, made for that screen's shape
    };
    std::vector<BoardSpot> boards; // persisted
    std::vector<std::wstring> boardsAway; // dashboards switched to the plain desktop for now, by the shortcut or their button; forgotten at restart
    BoardSpot* Spot(const std::wstring& screen);
    void SetBoardOn(const std::wstring& screen, bool on); // persisted
    void SetBoardTiles(const std::wstring& screen, const std::vector<BoardTile>& t); // persisted
    std::vector<BoardTile> BoardPreset(int rows) const; // the starting layout, for a grid this many rows deep
    int boardsUp = 0; // dashboards showing now: the stats they show keep coming
    std::vector<BoardTile> boardShown; // every tile on a showing dashboard, so polling follows what they need
    bool BoardHas(int kind) const;
    static const wchar_t* BoardName(int kind);
    static int BoardGlyph(int kind);
    void SetBoardLive(int up, std::vector<BoardTile> shown); // the dashboards came or went

    float binZone[4] = {}; // the shelf's recycle bin while something is dragged over the panel, panel space: left top right bottom
    bool binOver = false; // the drag is over it

private:
    bool panelLive = false, panelProcs = false, panelPing = false; // what the open panel asked for; the dashboard adds its own
    void ApplyLive();
    void LoadBoard(); // the dashboard's switch, screen and tiles
    void Worker();

    sys::NetSrc netSrc;
    sys::StatSrc statSrc;
    sys::ProcSrc procSrc;
    std::thread worker;
    std::mutex lock; // guards fields shared with worker
    std::condition_variable wake;
    bool quit = false, pollOn = false, wantProcs = false, ready = false; // ready = fresh sample waiting
    ULONGLONG listAt = 0; // worker only: when the process list was last walked, its cpu baseline
    bool wantPing = false; // the network page is showing: measure latency
    bool wantList = false; // the process list, costly: only the system page or a processes tile
    bool sysContrast = false; // windows' high contrast is on
    int sysText = 100; // windows' text size, percent
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
