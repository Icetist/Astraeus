#include "model.h"

#include <iphlpapi.h>
#include <icmpapi.h>
#include "faces.h"
#include "feeds.h"
#include "glyphs.h"
#include "sports.h"
#include "duration.h"
#include "pages.h"
#include "picker.h"
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <ctime>

namespace {

int ReadPower();

unsigned long long Ticks(const FILETIME& f) { // filetime -> 100ns count
    return (unsigned long long)f.dwHighDateTime << 32 | f.dwLowDateTime;
}

constexpr double kHour = 3600;
constexpr double kRecentFor = 2 * kHour; // a game over this long ago still counts as on, for the pill's score
constexpr size_t kSeenCap = 2000;

double GameLen(int sport) { // about how long one lasts, start to finish: for a game already over when it was first seen
    switch (sport) {
    case sports::kAmerican: return 3.5 * kHour;
    case sports::kBasketball: case sports::kHockey: case sports::kLacrosse: return 2.5 * kHour;
    case sports::kBaseball: case sports::kTennis: case sports::kAussie: return 3 * kHour;
    case sports::kFighting: case sports::kCricket: return 4 * kHour;
    case sports::kGolf: return 10 * kHour;
    case sports::kChess: return 5 * kHour;
    default: return 2 * kHour; // football, rugby, motorsport, volleyball
    }
}

}

namespace {
constexpr wchar_t kLookVal[] = L"Appearance";
constexpr int kLooks = Model::kLookChrome + 1; // dark, light, glass, custom, halobar's, starfall, clear, chrome
constexpr wchar_t kQuietVal[] = L"Quiet"; // do not disturb: halobar reads and writes the same value
constexpr wchar_t kArtBackVal[] = L"MediaArtBg"; // the album art background, shared with halobar the same way
constexpr wchar_t kVizAlphaVal[] = L"VizAlpha", kVizGainVal[] = L"VizGain"; // the visualizer's opacity over the art and its sensitivity, shared too
constexpr wchar_t kHaloKey[] = L"Software\\HaloBar"; // halobar's own settings, never written from here
constexpr wchar_t kHaloClass[] = L"HaloBar"; // its window: there means it's running
constexpr int kMaxDockReach = 400; // pixels: past this the value is junk, not a dock

constexpr float kQuietFloor = 0.02f; // below this it's silence, not quiet music: nothing is blown up from it
constexpr float kForget = 0.996f; // per read, a few seconds to let go of a loud moment
constexpr float kFullHead = 0.85f; // the loudest peak lands just under full, so the gain slider still has room
constexpr wchar_t kPagesVal[] = L"PageOrder", kTilesVal[] = L"ToolTiles"; // saved by name: a new page or tile later never shifts the old ones
std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t a = 0;
    for (size_t b; (b = s.find(L'\n', a)) != std::wstring::npos; a = b + 1) out.push_back(s.substr(a, b - a));
    if (a < s.size()) out.push_back(s.substr(a));
    return out;
}

const wchar_t* const kPageKeys[Model::kPageIds] = {L"home", L"media", L"system", L"network", L"tools", L"shelf", L"widgets"};
const wchar_t* const kTileKeys[Model::kToolTiles] = {L"screenshot", L"color", L"awake", L"notifications", L"dnd", L"saver", L"airplane", L"settings", L"workspaces", L"language", L"bin", L"updates"};
constexpr wchar_t kNoneVal[] = L"none"; // every tile taken off: an empty text would read as never set

std::vector<int> ParseList(const std::wstring& s, const wchar_t* const* keys, int n) { // comma separated names; unknown and repeated ones dropped
    std::vector<int> out;
    for (size_t at = 0; at <= s.size();) {
        size_t end = (std::min)(s.find(L',', at), s.size());
        std::wstring tok = s.substr(at, end - at);
        for (int i = 0; i < n; ++i)
            if (tok == keys[i] && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
        at = end + 1;
    }
    return out;
}

std::wstring JoinList(const std::vector<int>& v, const wchar_t* const* keys) {
    std::wstring s;
    for (int id : v) s += (s.empty() ? L"" : L",") + std::wstring(keys[id]);
    return s.empty() ? kNoneVal : s;
}

constexpr wchar_t kOurKey[] = L"Software\\Astraeus"; // where quiet and the art background live, halobar writes them too

struct KeyWatch { // a key's change notice: its values are read again after a write to it, not every second
    HKEY key = nullptr;
    HANDLE ev = nullptr;
    bool Changed(const wchar_t* path) { // true the first time, after any write, and every time while the key can't be watched (missing: read as before)
        if (!ev && !(ev = CreateEventW(nullptr, FALSE, FALSE, nullptr))) return true;
        bool fired;
        if (key) fired = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
        else if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_NOTIFY, &key) != ERROR_SUCCESS) return key = nullptr, true;
        else fired = true; // just opened: nothing read yet
        if (fired && RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, ev, TRUE) != ERROR_SUCCESS) // armed again before the read, so a write during it still counts
            RegCloseKey(key), key = nullptr; // deleted or gone: opened again next second
        return fired;
    }
};
KeyWatch ourWatch, haloWatch; // the ui thread's, for the process' life
bool haloThemeWas = false; // the theme was wanted at the last read

int HaloNum(HKEY key, const wchar_t* name, int fallback, int lo, int hi) { // out of range is stale or hand-edited: the default
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return fallback;
    int n = (int)v; // 0xFFFFFFFF reads back as -1, halobar's "follow"
    return n < lo || n > hi ? fallback : n;
}
}

Model::Model(const Config& c) : cfg(c), portable(sys::IsPortable()), look(sys::ReadNum(kLookVal, 0)) {
    if (look < 0 || look >= kLooks) look = 0; // unknown value: back to dark
    glassBlur = (std::clamp)(sys::ReadNum(L"GlassBlur", 100), 0, 100);
    glassTint = (std::clamp)(sys::ReadNum(L"GlassTint", -1), -1, 0xFFFFFF);
    customBg = sys::ReadNum(L"CustomBg", customBg) & 0xFFFFFF;
    lyricOffsetMs = 0; // per song now, remembered with its lyrics
    edge = (std::clamp)(sys::ReadNum(L"Edge", 0), 0, 2);
    screens = (std::clamp)(sys::ReadNum(L"Screens", 0), 0, 1);
    screen = sys::ReadText(L"Screen");
    islandSecs = (std::clamp)(sys::ReadNum(L"IslandSecs", 0), -1, kIslandMaxSecs);
    int iniHex = ((int)(cfg.themeCol.r * 255 + 0.5f) << 16) | ((int)(cfg.themeCol.g * 255 + 0.5f) << 8) | (int)(cfg.themeCol.b * 255 + 0.5f);
    accentMode = (std::clamp)(sys::ReadNum(L"AccentMode", cfg.wallTheme ? 0 : 1), 0, kAccentWindows); // settings override the ini once used
    if (accentMode == kAccentWindows) winAccentHex = winAccent.Read();
    accentHex = sys::ReadNum(L"AccentHex", iniHex) & 0xFFFFFF;
    notifyPills = (std::clamp)(sys::ReadNum(L"NotifyPills", 0), 0, 1);
    quiet = sys::ReadNum(kQuietVal, 0) ? 1 : 0; // missing means off
    fullLyrics = (std::clamp)(sys::ReadNum(L"FullLyrics", 1), 0, 1);
    mediaLyrics = (std::clamp)(sys::ReadNum(L"MediaLyrics", 0), 0, 1);
    fahr = (std::clamp)(sys::ReadNum(L"Fahrenheit", cfg.fahr ? 1 : 0), 0, 1);
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0); // windows' own "animation effects": off means start calm
    reduceMotion = (std::clamp)(sys::ReadNum(L"ReduceMotion", anim ? 0 : 1), 0, 1);
    auto flag = [](const wchar_t* key, int fallback) { return sys::ReadNum(key, fallback) ? 1 : 0; };
    powerPills = flag(L"PowerPills", 1);
    batLow = (std::clamp)(sys::ReadNum(L"BatteryLow", 20), 5, 50);
    activityBubble = flag(L"ActivityBubble", 1);
    rememberPage = flag(L"RememberPage", 1);
    defaultPage = (std::clamp)(sys::ReadNum(L"DefaultPage", 0), 0, kPageIds - 1); // identities: the numbers saved before the rail could change still mean the same pages
    lastPage = (std::clamp)(sys::ReadNum(L"LastPage", 0), 0, kPageIds - 1);
    pages = ParseList(sys::ReadText(kPagesVal), kPageKeys, kPageIds);
    if (pages.empty()) pages = DefaultPages();
    std::wstring tl = sys::ReadText(kTilesVal);
    tiles = tl.empty() ? DefaultTiles() : ParseList(tl, kTileKeys, kToolTiles);
    stageHide = flag(L"StageHide", 1);
    stageLyrBtn = flag(L"StageLyricsButton", 1);
    stageEditBtn = flag(L"StageEditButton", 1);
    tourNext = flag(L"TourNext", 1);
    stageThump = flag(L"StageThump", 1);
    uiScale = (std::clamp)(sys::ReadNum(L"UiScale", 100), 80, 130);
    a11yContrast = (std::clamp)(sys::ReadNum(L"A11yContrast", kContrastAuto), (int)kContrastAuto, (int)kContrastOff);
    a11yFollow = flag(L"A11yFollowText", 1);
    a11yText = (std::clamp)(sys::ReadNum(L"A11yText", kTextMin), kTextMin, kTextMax);
    a11yKeys = flag(L"A11yKeys", 1);
    a11yReader = flag(L"A11yReader", 1);
    ReadAccess();
    lyricScale = (std::clamp)(sys::ReadNum(L"LyricScale", 100), kLyricMin, kLyricMax);
    bgOpacity = (std::clamp)(sys::ReadNum(L"BgOpacity", kMaxOpacity), kMinOpacity, kMaxOpacity);
    auto appsNum = [](const wchar_t* key, int fallback) { return sys::ReadNum((std::wstring(key) + L"Apps").c_str(), fallback); }; // never set: the desktop's
    otherSet = {(std::clamp)(appsNum(kLookVal, look), 0, kLookChrome), (std::clamp)(appsNum(L"AccentMode", accentMode), 0, (int)kAccentWindows), appsNum(L"AccentHex", accentHex) & 0xFFFFFF,
                appsNum(L"CustomBg", customBg) & 0xFFFFFF, (std::clamp)(appsNum(L"GlassBlur", glassBlur), 0, 100), (std::clamp)(appsNum(L"BgOpacity", bgOpacity), kMinOpacity, kMaxOpacity),
                (std::clamp)(appsNum(L"GlassTint", glassTint), -1, 0xFFFFFF)};
    appsTheme = sys::ReadNum(L"AppsTheme", 0) != 0 ? 1 : 0;
    pillAlways = flag(L"PillAlways", 0);
    pillFull = flag(L"PillFullscreen", 0);
    pillFullOpen = flag(L"PillFullOpen", 1);
    pillDesk = flag(L"PillDesktop", 1); // on out of the box: the island stays in sight on an empty desktop
    pillLeft = (std::clamp)(sys::ReadNum(L"PillLeft", kWWeather), 0, kWidgets - 1);
    pillRight = (std::clamp)(sys::ReadNum(L"PillRight", kWCpu), 0, kWidgets - 1);
    int pillWas = sys::ReadNum(L"DeskPill", -1); // -1: never chosen
    if (pillWas < 0) { // an update keeps the regular pill they had; a new install starts on the long one. saved, so it never flips later
        pillWas = sys::ReadNum(L"LastPage", -1) >= 0 ? 0 : 1;
        sys::SaveNum(L"DeskPill", pillWas);
    }
    deskPill = pillWas != 0;
    deskFillApps = (std::clamp)(sys::ReadNum(L"DeskFillApps", sys::ReadNum(L"DeskFill", 0)), 0, 3);
    deskEdge = (std::clamp)(sys::ReadNum(L"DeskEdge", 0), 0, 2);
    deskPos = (std::clamp)(sys::ReadNum(L"DeskPos", 500), 0, 1000);
    deskLen = (std::clamp)(sys::ReadNum(L"DeskLen", 560), 60, 8000);
    deskThick = (std::clamp)(sys::ReadNum(L"DeskThick", 30), 24, 56);
    deskGap = (std::clamp)(sys::ReadNum(L"DeskGap", 10), 4, 120);
    deskFloat = flag(L"DeskFloat", 1);
    deskLenApps = (std::clamp)(sys::ReadNum(L"DeskLenApps", deskLen), 60, 8000); // never set: as it is on the desktop
    deskGapApps = (std::clamp)(sys::ReadNum(L"DeskGapApps", deskGap), 4, 120);
    deskFloatApps = flag(L"DeskFloatApps", deskFloat);
    deskThickApps = (std::clamp)(sys::ReadNum(L"DeskThickApps", deskThick), 24, 56); // never set: as it is on the desktop
    deskApps = flag(L"DeskApps", 0);
    deskFill = (std::clamp)(sys::ReadNum(L"DeskFill", 0), 0, 3);
    auto items = [&](const wchar_t* name, std::vector<int>& out) { // "-": taken all off on purpose; nothing saved: the defaults
        std::wstring saved = sys::ReadText(name);
        if (saved.empty()) return (void)(out = DeskDefaults());
        if (saved == L"-") return;
        for (size_t at = 0; at < saved.size();) {
            size_t end = saved.find(L',', at);
            if (end == std::wstring::npos) end = saved.size();
            int v = _wtoi(saved.substr(at, end - at).c_str());
            if (((v > kWNone && v < kWidgets) || (v >= kDClock && v <= kDLyrics)) && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
            at = end + 1;
        }
    };
    items(L"DeskItems", deskItems);
    deskSplit = flag(L"DeskSplit", 0);
    if (sys::ReadText(L"DeskItemsApps").empty()) deskItemsApps = deskItems; // never split yet: it starts as the desktop's
    else items(L"DeskItemsApps", deskItemsApps);
    deskMid = (std::max)(sys::ReadNum(L"DeskMid", kMidAuto), kMidNone); // never set: the clock keeps the middle, as it always has
    deskMidApps = (std::max)(sys::ReadNum(L"DeskMidApps", deskMid), kMidNone);
    mediaCompact = flag(L"MediaCompact", 0);
    pillMusic = flag(L"PillMusic", 1);
    showArt = flag(L"ShowArt", 1);
    desk = sys::LoadDesk();
    for (const auto& line : SplitLines(sys::ReadText(L"Todos"))) { // "1|text": done, then the words; a tab and yyyymmdd when it's due (typing can't make a tab)
        if (line.size() <= 2 || line[1] != L'|' || (int)todos.size() >= kMaxTodos) continue;
        std::wstring text = line.substr(2);
        int due = 0;
        if (size_t tab = text.rfind(L'\t'); tab != std::wstring::npos) due = _wtoi(text.c_str() + tab + 1), text.resize(tab);
        todos.push_back({text, line[0] == L'1', due > 19000101 && due < 30000101 ? due : 0});
    }
    for (const auto& line : SplitLines(sys::ReadText(L"Servers")))
        if (!line.empty() && (int)hosts.size() < kMaxHosts) hosts.push_back(line);
    alarms.Deserialize(sys::ReadText(L"Alarms"));
    if (std::wstring seen = sys::ReadText(L"AlarmSeen"); !seen.empty()) {
        try { alarms.lastCheck = std::stoll(seen); } catch (...) {}
    }
    vizOn = flag(L"Visualizer", cfg.viz ? 1 : 0); // the ini's choice until the switch in settings is used
    artBack = flag(kArtBackVal, 0);
    stageArt = flag(L"StageArtBg", 0);
    vizAlpha = (std::clamp)(sys::ReadNum(kVizAlphaVal, 60), kVizAlphaMin, 100);
    vizGain = (std::clamp)(sys::ReadNum(kVizGainVal, 100), kVizGainMin, kVizGainMax);
    keyOnly = flag(L"KeyOnly", 0);
    appKeyOnly = flag(L"AppKeyOnly", 0);
    appHoverMs = (std::clamp)(sys::ReadNum(L"AppHoverMs", kAppHoverMs), 0, kAppHoverMax);
    hud = flag(L"Hud", 0); // experimental: off unless asked for
    vizWave = flag(L"VizWave", 0);
    vizFull = flag(L"VizFull", 0);
    layout = (std::clamp)(sys::ReadNum(L"Layout", kLayHorizontal), 0, kLayouts - 1);
    pinned = flag(L"Pinned", 0);
    floating = flag(L"Floating", 0);
    LoadBoard();
    shelfCapture = flag(L"ShelfCapture", 0); // off unless asked for: the shelf holds only what's dropped on it
    shelfHistory = (std::clamp)(sys::ReadNum(L"ShelfHistory", sys::Shelf::kHistDef), sys::Shelf::kHistMin, sys::Shelf::kHistMax);
    shelfExpire = (std::clamp)(sys::ReadNum(L"ShelfExpire", sys::Shelf::kExpireNever), 0, sys::Shelf::kExpireNever);
    shelfClearStart = flag(L"ShelfClearStart", 0);
    SyncShelf(); // before Start: a clear at launch needs to know
}

void Model::SyncShelf() { shelf.Configure({shelfCapture != 0, shelfHistory, shelfExpire, shelfClearStart != 0}); }

void Model::CommitShelfNote() {
    if (shelfField == kShelfNote && shelfNoteFor) shelf.SetNote(shelfNoteFor, shelfEdit);
    shelfField = kShelfNone;
    shelfNoteFor = 0;
    shelfEdit.clear();
}

Model::~Model() {
    SaveAlarmSeen();
    {
        std::lock_guard<std::mutex> guard(lock);
        quit = true; // set under lock so worker sees it
    }
    wake.notify_all(); // break worker out of wait
    if (worker.joinable()) worker.join();
    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
}

void Model::Start(HWND hwnd) {
    notify = hwnd;
    share.Notify(hwnd, WM_APP_SHARE);
    PollShared(); // halobar may already be handing its pills over, and its theme may be ours
    media.Start(hwnd, WM_APP_MEDIA);
    radios.Start(hwnd, WM_APP_RADIOS);
    cals.Sync(hwnd, WM_APP_CAL, 0); // the month's dots are ready by the time the calendar first opens
    shelf.Start(hwnd, WM_APP_SHELF); // a kept shelf comes back
    if (accentMode == kAccentWindows) winAccent.Watch(hwnd, WM_APP_ACCENT);
    if (portable) ReadBattery();
    CheckTiler(); // a tiling manager here: its workspaces tile comes on, unless taken off since
    { // the widgets picked, by name: a catalogue that grows never shifts them
        std::wstring saved = sys::ReadText(L"Widgets");
        for (size_t a = 0; a < saved.size();) {
            size_t e = (std::min)(saved.find(L',', a), saved.size());
            size_t colon = (std::min)(saved.find(L':', a), e); // "key" or "key:size"
            std::string key;
            for (size_t i = a; i < colon; ++i) key += (char)saved[i];
            int k = faces::Find(key.c_str());
            if (k >= 0 && std::find(faceOn.begin(), faceOn.end(), k) == faceOn.end()) {
                faceOn.push_back(k);
                if (colon + 1 < e) faceSize[k] = (std::clamp)(saved[colon + 1] - L'0', 0, 2);
            }
            a = e + 1;
        }
    }
    SyncFaces();
    Sample(0); // seed cpu/net baselines, pushes nothing
    worker = std::thread([this] { Worker(); });
}

void Model::SetLive(bool active, bool processes, bool ping) {
    if (!active) CloseMixer(); // the audio sessions and icons go with the panel
    bool woke = active != panelLive;
    panelLive = active;
    if (woke) {
        if (active) CheckTiler(); // installed or removed while it was away
        SyncFaces();
    }
    panelProcs = active && processes;
    panelPing = active && ping;
    ApplyLive();
}

void Model::SetBoardLive(int up, std::vector<BoardTile> shownTiles) {
    boardsUp = up;
    boardShown = std::move(shownTiles);
    ApplyLive();
}

namespace sys {
void GpuWanted(int every); // sys_stats.cpp: seconds between reads of windows' costly gpu counter, 0 to close it
}

void Model::SetPillMeters(bool gpu, bool meters) {
    pillMeters = meters;
    if (gpu == pillGpu) return;
    pillGpu = gpu;
    ApplyLive();
}

void Model::ApplyLive() { // the panel's wishes and the dashboards', together
    constexpr int kBoardGpuSec = 5; // a dashboard's gpu gauge: a look every few seconds
    constexpr int kPillGpuSec = 2; // a pill's: lively, without paying for windows' costly counter every second
    sys::GpuWanted(panelProcs || pillGpu ? kPillGpuSec : boardsUp > 0 && BoardHas(kBdGpu) ? kBoardGpuSec : 0); // the system page reads it every second, nobody else at all
    bool up = boardsUp > 0, gauges = up && (BoardHas(kBdCpu) || BoardHas(kBdGpu) || BoardHas(kBdRam) || BoardHas(kBdProcs)); // the gauges come from the worker's sample
    bool list = panelProcs || (up && BoardHas(kBdProcs)); // walking every process is the costly part: only where a list shows it
    bool active = panelLive || up || pillGpu, processes = panelProcs || gauges || pillGpu, ping = panelPing; // the pill's reading comes from the worker's sample too; the process list stays off
    shown = panelLive || (up && (BoardHas(kBdCpu) || BoardHas(kBdRam) || BoardHas(kBdNet))); // ui thread: graphs sampled every second only while one shows
    {
        std::lock_guard<std::mutex> guard(lock);
        bool wakeNow = active && ((processes && !wantProcs) || (list && !wantList) || (ping && !wantPing)); // wake only if the system or network page just came up; the list too, when the pill's meters already had it sampling
        pollOn = active;
        wantProcs = active && processes;
        wantList = active && list;
        wantPing = active && ping;
        if (!wakeNow) return; // otherwise next 1s cycle picks it up
    }
    wake.notify_all(); // notify outside lock, avoids instant re-block
}

void Model::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // stats never starve ui
    THREAD_POWER_THROTTLING_STATE eco{THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, THREAD_POWER_THROTTLING_EXECUTION_SPEED};
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &eco, sizeof(eco)); // walking the process list runs on efficient cores and low clocks
    std::unique_lock<std::mutex> lk(lock);
    while (!quit) {
        wake.wait(lk, [this] { return quit || (pollOn && (wantProcs || wantPing)); }); // asleep unless the system page (gauges, processes) or the network page (latency) shows
        if (quit) break;
        bool procsNow = wantProcs, pingNow = wantPing, listNow = wantList; // snapshot, flags may change while unlocked
        lk.unlock(); // slow sampling runs without lock
        if (listNow) { // the list first, and sent at once: the gpu counter below can take a while
            constexpr ULONGLONG kStaleMs = 3000, kPrimeMs = 250; // an old baseline, or none: a quick one, so the shares are real from the start
            ULONGLONG at = GetTickCount64();
            if (at - listAt > kStaleMs) procSrc.Top(6), Sleep((DWORD)kPrimeMs);
            listAt = GetTickCount64();
            std::vector<sys::ProcRow> top = procSrc.Top(6); // top 6 procs, only where a list shows them
            lk.lock();
            newProcs = std::move(top);
            ready = true;
            lk.unlock();
            PostMessageW(notify, WM_APP_STATS, 0, 0);
        }
        sys::Stats s = procsNow ? statSrc.Sample() : sys::Stats{}; // the gpu counter is costly: only where its gauge is
        float ping = -2;
        if (pingNow) { // one echo to a public resolver, only while the network page shows
            if (icmp == INVALID_HANDLE_VALUE) icmp = IcmpCreateFile();
            char reply[sizeof(ICMP_ECHO_REPLY) + 32];
            char data[8] = {'a', 's', 't', 'r', 'a', 'e', 'u', 's'};
            IPAddr to = 0x01010101; // 1.1.1.1
            DWORD n = icmp != INVALID_HANDLE_VALUE ? IcmpSendEcho(icmp, to, data, sizeof(data), nullptr, reply, sizeof(reply), 900) : 0;
            const auto* r = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply);
            ping = n && r->Status == IP_SUCCESS ? (float)r->RoundTripTime : -1;
        }
        lk.lock();
        newStats = s;
        newPing = ping;
        ready = true;
        PostMessageW(notify, WM_APP_STATS, 0, 0); // ui thread pulls via OnStats
        wake.wait_for(lk, std::chrono::seconds(1), [this, procsNow, listNow] { return quit || (wantProcs && !procsNow) || (wantList && !listNow); }); // 1s cadence, early wake if procs or the list newly wanted
    }
}

void Model::OnStats() {
    std::lock_guard<std::mutex> guard(lock);
    if (!ready) return; // stale or duplicate post
    ready = false;
    if (wantProcs) { // the system page shows: its gauges and process list
        stats = newStats;
        procs = newProcs;
    }
    if (newPing > -2) {
        if (newPing >= 0 && pingMs >= 0) jitterMs = jitterMs * 0.8f + std::fabs(newPing - pingMs) * 0.2f; // how much it wobbles, smoothed
        pingMs = newPing;
        pingHist.Push((std::max)(0.0f, newPing));
        newPing = -2;
    }
}

void Model::Tick() {
    if (timerOn && GetTickCount64() >= timerEnd) { // timer checked at tick granularity
        timerOn = false;
        timerDone = true; // ui consumes via TimerDone()
    }
    DeskTick(1, false); // apps come and go; groups keep their levels
    SyncExtras();
    if ((fullLyrics || mediaLyrics || PillLyrics()) && hasTrack && track.music && track.duration > 0) // the song's lyrics, fetched while it plays: ready long before fullscreen opens; a cache check once known
        lyrics.Want(track.title, track.artist, track.album, track.duration / 1e7);
    if (focusOn && GetTickCount64() >= focusEnd) { // a block ends: focus hands over to a break, a break to the next round
        focusEvent = focusBreak ? 2 : 1;
        if (focusBreak) ++focusRound;
        focusBreak = !focusBreak;
        focusLen = (ULONGLONG)(focusBreak ? breakMin : focusMin) * 60000;
        focusEnd = GetTickCount64() + focusLen;
    }
    constexpr int kClosedEvery = 5, kPillEvery = 2; // closed: sample every 5s, nobody is looking at the graphs; a pill reading cpu or ram stays lively
    if (++unsampled < (shown ? 1 : pillMeters ? kPillEvery : kClosedEvery)) return;
    Sample(unsampled);
    unsampled = 0;
}

void Model::Sample(int seconds) {
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        unsigned long long i = Ticks(idle), t = Ticks(kernel) + Ticks(user); // kernel time includes idle
        if (lastTotal != 0 && t > lastTotal) { // busy = 1 - idle delta / total delta, averaged over the gap
            float busy = 1.0f - (float)(i - lastIdle) / (float)(t - lastTotal);
            for (int k = 0; k < seconds; ++k) cpuHist.Push(busy);
        }
        lastIdle = i;
        lastTotal = t;
    }
    MEMORYSTATUSEX m = {sizeof(m)}; // dwLength must be set first
    if (GlobalMemoryStatusEx(&m))
        for (int k = 0; k < seconds; ++k) ramHist.Push(m.dwMemoryLoad / 100.0f); // percent -> 0..1
    static int netInfo = 0; // the adapter and the wi-fi name: every second while the panel shows them, else every ten
    net = netSrc.Sample(panelLive || ++netInfo % 10 == 0); // rates are averaged over the gap by the sampler's own clock
    for (int k = 0; k < seconds; ++k) {
        upHist.Push((float)net.upBps);
        downHist.Push((float)net.downBps);
    }
}

void Model::Prefetch() {
    if (cfg.weather) weather.Refresh(notify, WM_APP_WEATHER);
}

void Model::OnOpen() {
    spk.Refresh(); // device may have changed while hidden
    mic.Refresh();
    ReadAudio();
    if (portable) pwrMode = ReadPower(), ReadBattery(); // laptop shows power mode, desktop shows outputs
    else outs = sys::ListOuts(outId);
    if (vizOn) meter.Refresh();
    bright.ReadAsync(notify, WM_APP_BRIGHT); // ddc/wmi is slow, result posted back
    if (cfg.weather) weather.Refresh(notify, WM_APP_WEATHER);
    if (cfg.publicIp) netSrc.FetchWan(notify, WM_APP_WANIP); // async public ip lookup
    radios.Recheck(); // a bluetooth or wi-fi radio that wasn't ready at start shows up now, not "unavailable" all session
}

void Model::ReadAudio() {
    if (spk.Ready()) {
        vol = spk.Level();
        muted = spk.Muted();
    }
    if (mic.Ready()) {
        micVol = mic.Level();
        micMuted = mic.Muted();
    }
}

void Model::OnMedia() {
    hasTrack = media.Snapshot(track);
    media.Sources(mediaSources, mediaPin);
    if ((fullLyrics || mediaLyrics || PillLyrics()) && hasTrack && track.music && track.duration > 0) lyrics.Want(track.title, track.artist, track.album, track.duration / 1e7); // ready before fullscreen opens; no-op once known
}

void Model::OnBright(WPARAM value) {
    brightPct = value == (WPARAM)-1 ? -1 : (int)value;
}

void Model::OnWx() {
    wx = weather.Get();
}

void Model::OpenBt() {
    if (btOpen) return;
    CloseWifi(); // one sheet at a time
    CloseSettings();
    CloseCal();
    CloseMixer();
    btOpen = true;
    bt.Open(notify, WM_APP_BT);
    OnBt();
    OnShare(); // reopened on the shared-audio tab
}

void Model::CloseBt() {
    if (!btOpen) return;
    btOpen = btScan = false;
    bt.Close();
    btDevs.clear();
    btDevs.shrink_to_fit(); // give the memory back
}

void Model::OnBt() {
    if (!btOpen) return; // late posts after close
    bt.Snap(btDevs);
    btScan = bt.Scanning();
}

void Model::OnShare() {
    if (!btOpen || !shareTab) return; // listing probes every endpoint, only worth it while visible
    shareOuts = share.List();
}

void Model::OpenWifi() {
    if (wifiOpen) return;
    CloseBt(); // one sheet at a time
    CloseSettings();
    CloseCal();
    CloseMixer();
    wifiOpen = true;
    wifi.Open(notify, WM_APP_WIFI);
    OnWifi();
}

void Model::AskPass(const sys::WifiNet& n) {
    CancelPass();
    passFor = n.raw;
    passEdit.reserve(64); // never regrows, so no freed buffer holds part of a password
    passFocus = true;
}

void Model::SubmitPass() {
    for (const auto& n : wifiNets)
        if (n.raw == passFor && passEdit.size() >= 8) {
            if (!wifi.Join(n, passEdit)) sys::OpenUri(L"ms-availablenetworks:"); // wipes what was typed; couldn't start: windows' own flyout takes it from here
            passFocus = passWrong = passShow = false;
            passFor.clear();
            OnWifi();
            return;
        }
}

void Model::CancelPass() {
    if (!passEdit.empty()) SecureZeroMemory(passEdit.data(), passEdit.size() * sizeof(wchar_t)); // passwords don't linger
    passEdit.clear();
    passFor.clear();
    passFocus = passShow = passWrong = false;
}

void Model::CloseWifi() {
    CancelPass();
    if (!wifiOpen) return;
    wifiOpen = wifiScan = false;
    wifi.Close(); // list stays until the fade-out ends, see DropWifiList
}

void Model::ReopenWifi() {
    if (!wifiOpen) return;
    wifi.Close();
    wifi.Open(notify, WM_APP_WIFI);
}

void Model::DropWifiList() {
    if (wifiOpen || wifiNets.empty()) return;
    wifiNets.clear();
    wifiNets.shrink_to_fit(); // give the memory back
}

void Model::OnWifi() {
    if (!wifiOpen) return; // late posts after close
    wifi.Snap(wifiNets);
    wifiScan = wifi.Scanning();
    wifiState = wifi.State();
}

void Model::SetVol(float v) {
    vol = v;
    spk.SetLevel(v);
    if (v > 0) muted = false;
}

void Model::ToggleMute() {
    muted = !muted;
    spk.SetMuted(muted);
}

void Model::SetMic(float v) {
    micVol = v;
    mic.SetLevel(v);
    if (v > 0) micMuted = false;
}

void Model::ToggleMic() {
    micMuted = !micMuted;
    mic.SetMuted(micMuted);
}

void Model::SetBright(float v) {
    brightPct = (int)(v * 100 + 0.5f);
    bright.Set(brightPct);
}

void Model::SetAwake(bool on) {
    awake = on;
    sys::SetAwake(on);
}

void Model::SetPref(int& field, const wchar_t* key, int value) {
    field = value;
    sys::SaveNum(ThemeKey(key).c_str(), value);
    if (&field == &pillLeft || &field == &pillRight || &field == &deskPill || &field == &pillDesk || &field == &deskApps) SyncFaces(); // a pill may show the market now: it's fetched, or let go
}

std::wstring Model::ThemeKey(const wchar_t* key) const {
    if (setNow == 1)
        for (const wchar_t* k : {kLookVal, L"AccentMode", L"AccentHex", L"CustomBg", L"GlassBlur", L"BgOpacity", L"GlassTint"})
            if (!wcscmp(key, k)) return std::wstring(key) + L"Apps";
    return key;
}

void Model::SyncThemeSet() {
    int want = !appsTheme ? 0 : deskPreview >= 0 ? deskPreview : settingsOpen && setTab >= 0 ? setTab : overApp ? 1 : 0; // the long pill's editor shows the theme of the one being edited, whatever is behind it
    if (want == setNow) return;
    if (hexFocus) CommitHex(); // what's being typed belongs to the one going out
    ThemeSet live{look, accentMode, accentHex, customBg, glassBlur, bgOpacity, glassTint};
    look = otherSet.look, accentMode = otherSet.accentMode, accentHex = otherSet.accentHex, customBg = otherSet.customBg;
    glassBlur = otherSet.glassBlur, bgOpacity = otherSet.bgOpacity, glassTint = otherSet.glassTint;
    otherSet = live;
    setNow = want;
    if (accentMode == kAccentWindows) { // windows' accent: read and watched only by a theme that uses it
        winAccentHex = winAccent.Read();
        winAccent.Watch(notify, WM_APP_ACCENT);
    } else {
        winAccent.Stop();
    }
    PollShared(); // following halobar: its theme now
}

int Model::SlotOf(int id) const {
    auto it = std::find(pages.begin(), pages.end(), id);
    return it == pages.end() ? -1 : (int)(it - pages.begin());
}

int Model::OpenPage() const {
    int want = rememberPage ? lastPage : defaultPage;
    return HasPage(want) ? want : pages.front();
}

void Model::SetPages(const std::vector<int>& order) {
    if (order.empty()) return; // one page always stays
    pages = order;
    sys::SaveText(kPagesVal, JoinList(pages, kPageKeys));
}

std::vector<int> Model::DefaultPages() { return {kPgHome, kPgMedia, kPgSystem, kPgNetwork, kPgShelf, kPgTools}; } // every page, the shelf above the tools
std::vector<int> Model::DefaultTiles() { return {kTlSnip, kTlColor, kTlAwake, kTlDnd, kTlSettings}; } // the tiles the tools page always had

void Model::SetTiles(const std::vector<int>& order) {
    tiles = order;
    sys::SaveText(kTilesVal, JoinList(tiles, kTileKeys));
}

const wchar_t* Model::PageName(int id) {
    static const wchar_t* const kNames[kPageIds] = {L"Home", L"Media", L"System", L"Network", L"Tools", L"Shelf", L"Widgets"};
    return kNames[(std::clamp)(id, 0, kPageIds - 1)];
}

const wchar_t* Model::PageAbout(int id) {
    static const wchar_t* const kAbout[kPageIds] = {L"Your overview at a glance", L"What's playing and its controls", L"CPU, memory and what's running", L"Speed and what's connected", L"Power, timers and quick tiles", L"Files kept close at hand", L"Your board of widgets"};
    return kAbout[(std::clamp)(id, 0, kPageIds - 1)];
}

int Model::PageGlyph(int id) {
    static const int kGlyphs[kPageIds] = {glyph::Overview, glyph::Music, glyph::System, glyph::Network, glyph::Tools, glyph::Shelf, glyph::Dashboard};
    return kGlyphs[(std::clamp)(id, 0, kPageIds - 1)];
}

const wchar_t* Model::TileName(int id) {
    static const wchar_t* const kNames[kToolTiles] = {L"Screenshot", L"Color picker", L"Awake", L"Notifications", L"Do not disturb", L"Energy saver", L"Airplane", L"Settings", L"Workspaces", L"Keyboard language", L"Recycle bin", L"Updates"};
    return kNames[(std::clamp)(id, 0, kToolTiles - 1)];
}

const wchar_t* Model::TileAbout(int id) {
    static const wchar_t* const kAbout[kToolTiles] = {L"Pick a region of the screen", L"Pick a color from the screen", L"Keep the screen on", L"Open the notification list", L"Silence notifications", L"Turn energy saver on or off", L"Turn every radio off", L"Astraeus' settings", L"Switch between workspaces", L"Switch keyboard language", L"See and empty the bin", L"Check for updates"};
    return kAbout[(std::clamp)(id, 0, kToolTiles - 1)];
}

int Model::TileGlyph(int id) {
    static const int kGlyphs[kToolTiles] = {glyph::Snip, glyph::Edit, glyph::Coffee, glyph::Bell, glyph::Dnd, glyph::Saver, glyph::Airplane, glyph::Settings, glyph::Workspaces, glyph::Translate, glyph::Delete, glyph::Update};
    return kGlyphs[(std::clamp)(id, 0, kToolTiles - 1)];
}

void Model::OpenSettings() {
    if (settingsOpen) return;
    CloseBt(); // one sheet at a time
    CloseWifi();
    CloseCal();
    CloseMixer();
    settingsOpen = true;
    CheckTiler();
    screenList = sys::ListScreens(); // monitors may have changed since last time
    PollShared(); // the halobar swatch shows what it wears now
}

void Model::PickScreen(const std::wstring& name) {
    screen = name;
    sys::SaveText(L"Screen", name);
    SetPref(screens, L"Screens", 0);
}

void Model::OpenCal() {
    if (calOpen) return;
    CloseBt(); // one sheet at a time
    CloseWifi();
    CloseSettings();
    CloseMixer();
    calOpen = calFresh = true;
    calAgenda = calLinks = false;
    cals.Sync(notify, WM_APP_CAL, sys::cal::Calendars::kOnOpen); // changes made a minute ago show; no timer runs while it's closed
    calForecast = false;
}

void Model::SwToggle() {
    ULONGLONG now = GetTickCount64();
    if (swOn) swAccum += now - swStart;
    else swStart = now;
    swOn = !swOn;
}

void Model::SwReset() {
    swOn = false;
    swAccum = 0;
    laps.clear();
}

void Model::SwLap() {
    if (SwMs() == 0) return;
    laps.insert(laps.begin(), SwMs()); // newest first
    if (laps.size() > 20) laps.pop_back();
}

void Model::StartFocus(int focus, int rest) {
    focusMin = focus;
    breakMin = rest;
    focusOn = true;
    focusBreak = false;
    focusRound = 1;
    focusLen = (ULONGLONG)focus * 60000;
    focusEnd = GetTickCount64() + focusLen;
}

void Model::StopFocus() { focusOn = false; }

void Model::SkipFocus() {
    if (focusOn) focusEnd = GetTickCount64(); // the next tick moves on
}

double Model::FocusLeft() const { return focusOn ? (std::max)(0.0, (double)(long long)(focusEnd - GetTickCount64()) / 1000.0) : 0; }

double Model::FocusFrac() const { return focusOn && focusLen ? 1 - FocusLeft() * 1000.0 / focusLen : 0; }

void Model::StartPicker() { sys::picker::Start(notify, WM_APP_PICKED); }

void Model::OpenMixer() {
    if (mixerOpen) return;
    CloseBt(); // one sheet at a time
    CloseWifi();
    CloseSettings();
    CloseCal();
    mixerOpen = true;
    ReadAudio(); // the master and microphone faders start where windows has them
    mics = sys::ListIns(micId);
    if (!portable) outs = sys::ListOuts(outId);
    else if (outs.empty()) outs = sys::ListOuts(outId); // the mixer offers outputs per app on laptops too
    mixer.Open();
}

void Model::CloseMixer() {
    mixerOpen = false;
    deskEdit = nameFor = -1;
    if (!deskEngine.Owner() || desk.groups.empty()) mixer.Close(); // the sessions and icons go with it, unless groups still need them
}

void Model::DeskTick(float dt, bool fast) {
    if (!fast && sys::DeskChanged()) desk = sys::LoadDesk(), deskEngine.Forget(); // halobar edited it
    bool engine = !desk.groups.empty() && deskEngine.Claim();
    bool want = mixerOpen || engine;
    if (want && !mixer.IsOpen()) mixer.Open();
    else if (!want && mixer.IsOpen()) mixer.Close();
    if (!mixer.IsOpen()) return;
    if (!fast) mixer.Refresh();
    if (fast || deskEngine.NeedsMeter(desk)) mixer.Meter();
    if (engine) deskEngine.Tick(mixer, desk, dt);
    if (!fast && engine) sys::SaveDesk(desk); // the engine learns of levels moved elsewhere; written only when something changed
}

void Model::DeskSave() {
    sys::SaveDesk(desk);
    if (deskEngine.Owner()) deskEngine.Tick(mixer, desk, 0); // halobar applies it otherwise, within a second
}

void Model::DeskKey(int k) {
    std::wstring say;
    int groups = (int)desk.groups.size();
    if (k == 0) {
        ToggleMic();
        say = micMuted ? L"Microphone muted" : L"Microphone on";
    } else if (k == 1 && !desk.profiles.empty()) {
        desk.Recall((desk.profile + 1) % (int)desk.profiles.size());
        say = desk.profiles[desk.profile].name;
    } else if ((k == 2 || k == 3) && desk.Chatting()) {
        constexpr float kChatStep = 0.1f;
        desk.chatMix = (std::clamp)(desk.chatMix + (k == 3 ? kChatStep : -kChatStep), -1.0f, 1.0f);
        int pct = (int)std::lround(std::fabs(desk.chatMix) * 100);
        say = pct == 0 ? L"Chat mix even" : L"Chat mix " + std::to_wstring(pct) + L"% " + desk.groups[desk.chatMix > 0 ? desk.game : desk.chat].name;
    } else if (k >= 4 && k - 4 < groups) {
        auto& g = desk.groups[k - 4];
        g.muted = !g.muted;
        say = g.name + (g.muted ? L" muted" : L" on");
    } else {
        return;
    }
    DeskSave();
    if (desk.osd) deskSay = say;
}

void Model::CheckTiler() {
    constexpr ULONGLONG kTilerEveryMs = 60 * 1000; // one process list a minute at most, and only as the panel or settings opens
    ULONGLONG now = GetTickCount64();
    if (tilerAt && now - tilerAt < kTilerEveryMs) return;
    tilerAt = now;
    sys::Tiler found = sys::FindTiler();
    if (found == tiler) return;
    bool had = tiler != sys::Tiler::kNone;
    tiler = found;
    if (!had && std::find(tiles.begin(), tiles.end(), (int)kTlWork) == tiles.end() && !sys::ReadNum(L"WorkTileOff", 0)) { // just found: the tile comes on
        tiles.insert(tiles.begin(), kTlWork);
        SetTiles(tiles);
    }
    if (notify) SyncExtras();
}

bool Model::WantsWs() const {
    if (tiler == sys::Tiler::kNone) return false; // no tiling manager: nothing reads desktops on its behalf
    if (pillLeft == kWWork || pillRight == kWWork) return true;
    if (AnyLong() && std::find(deskItems.begin(), deskItems.end(), (int)kWWork) != deskItems.end()) return true;
    if (boardsUp && BoardHas(kBdWork)) return true;
    return std::find(tiles.begin(), tiles.end(), (int)kTlWork) != tiles.end();
}

void Model::SyncExtras() {
    bool want = WantsWs();
    if (want && !ws.On()) ws.Start(notify, WM_APP_WS);
    else if (!want && ws.On()) ws.Stop(), wsNow = {};
    if (!hosts.empty() && !pinger.On()) pinger.Start(notify, WM_APP_PING, hosts); // a minute between looks: nothing to save by stopping it
    constexpr int kUpdateFirst = 60, kUpdateEvery = 6 * 3600; // a minute after start, then every six hours
    bool tile = std::find(tiles.begin(), tiles.end(), (int)kTlUpdates) != tiles.end();
    if (tile && (++updateTick == kUpdateFirst || updateTick % kUpdateEvery == 0)) updates.Check(notify, WM_APP_UPDATES);
}

void Model::SaveTodos() {
    std::wstring s;
    for (const auto& t : todos) s += (t.done ? L"1|" : L"0|") + t.text + (t.due ? L"\t" + std::to_wstring(t.due) : L"") + L"\n";
    sys::SaveText(L"Todos", s);
}

int Model::TodoToday() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

int Model::TodoAddDays(int ymd, int days) { // through filetime: months and years roll over on their own
    constexpr LONGLONG kDay = 864000000000LL; // 100 ns ticks
    SYSTEMTIME st{};
    st.wYear = (WORD)(ymd / 10000), st.wMonth = (WORD)(ymd / 100 % 100), st.wDay = (WORD)(ymd % 100);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return ymd;
    ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
    u.QuadPart += days * kDay;
    ft = {u.LowPart, u.HighPart};
    FileTimeToSystemTime(&ft, &st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

bool Model::TodoDueOn(int y, int mo, int d) const {
    int ymd = y * 10000 + mo * 100 + d;
    return std::any_of(todos.begin(), todos.end(), [&](const Todo& t) { return !t.done && t.due == ymd; });
}

void Model::SaveHosts() {
    std::wstring s;
    for (const auto& h : hosts) s += h + L"\n";
    sys::SaveText(L"Servers", s);
    pinger.Stop();
    hostNow.clear();
    if (!hosts.empty()) pinger.Start(notify, WM_APP_PING, hosts);
}

void Model::SaveAlarms() { sys::SaveText(L"Alarms", alarms.Serialize()); }

void Model::SaveAlarmSeen() {
    sys::SaveText(L"AlarmSeen", alarms.items.empty() ? L"" : std::to_wstring(alarms.lastCheck));
}

void Model::BeginAlarmEdit(int id) {
    alarmEditId = id;
    if (id > 0) {
        if (const Alarm* a = alarms.Find(id)) {
            alarmHour = a->hour;
            alarmMin = a->minute;
            alarmDaily = a->daily;
            alarmLabel = a->label;
            return;
        }
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int total = st.wHour * 60 + st.wMinute + 15;
    alarmHour = (total / 60) % 24;
    alarmMin = total % 60;
    alarmDaily = false;
    alarmLabel.clear();
}

void Model::SaveAlarmEditor() {
    if (field == kFieldAlarmLabel) {
        alarmLabel = AlarmStore::SanitizeLabel(fieldEdit);
        field = kFieldNone;
        fieldEdit.clear();
    }
    bool ok = alarmEditId > 0 ? alarms.Update(alarmEditId, alarmHour, alarmMin, alarmDaily, alarmLabel)
                              : alarms.Add(alarmHour, alarmMin, alarmDaily, alarmLabel) > 0;
    if (ok) {
        if (alarms.lastCheck <= 0) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            alarms.lastCheck = LocalEpochSec(now);
        }
        SaveAlarms();
        SaveAlarmSeen();
    }
    alarmEditId = -1;
}

void Model::CancelAlarmEditor() {
    if (field == kFieldAlarmLabel) field = kFieldNone, fieldEdit.clear();
    alarmEditId = -1;
}

bool Model::TickAlarms() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    int64_t nowSec = LocalEpochSec(now);
    bool wasMissed = alarms.HasMissed();
    bool wasRinging = alarms.ringingId != 0;
    int fired = alarms.Tick(nowSec, now);
    if (fired > 0 || alarms.HasMissed() != wasMissed || (wasRinging && alarms.ringingId == 0)) {
        SaveAlarms();
        SaveAlarmSeen();
        alarmSeenTick = 0;
    } else if (!alarms.items.empty() && ++alarmSeenTick >= 30) {
        alarmSeenTick = 0;
        SaveAlarmSeen();
    }
    return fired > 0;
}

bool Model::SnoozeAlarm() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (!alarms.Snooze(LocalEpochSec(now))) return false;
    alarmBellTick = 0;
    SaveAlarms();
    SaveAlarmSeen();
    return true;
}

bool Model::DismissAlarm() {
    if (!alarms.Dismiss()) return false;
    alarmBellTick = 0;
    SaveAlarms();
    SaveAlarmSeen();
    return true;
}

void Model::CommitField() {
    if (field == kFieldTimer) {
        if (int seconds = duration::Parse(fieldEdit)) StartTimer(seconds), field = kFieldNone, fieldEdit.clear(); // not a time: it stays, to be corrected
        return;
    }
    if (field == kFieldAlarmLabel) {
        alarmLabel = AlarmStore::SanitizeLabel(fieldEdit);
        field = kFieldNone;
        fieldEdit.clear();
        return;
    }
    std::wstring s = fieldEdit;
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    s = a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
    for (auto& c : s)
        if (c == L'\n' || c == L'\n') c = L' ';
    if (!s.empty() && field == kFieldTodo && (int)todos.size() < kMaxTodos) todos.push_back({s}), SaveTodos();
    if (!s.empty() && field == kFieldHost && (int)hosts.size() < kMaxHosts) hosts.push_back(s), SaveHosts();
    fieldEdit.clear(); // the field stays: the next one can be typed straight away
}

void Model::CommitName() {
    std::wstring s = nameEdit;
    while (!s.empty() && s.back() == L' ') s.pop_back();
    if (nameFor >= 0 && nameFor < (int)desk.groups.size() && !s.empty()) desk.groups[nameFor].name = s, DeskSave();
    nameFor = -1;
}

void Model::CloseCal() { calOpen = false; }

void Model::StartCalWait(bool google) {
    if (!calWaiting) AddClipboardFormatListener(notify); // the link the user copies next, from any app, is the one
    calWaiting = true;
    SetTimer(notify, kCalWaitTimer, 5 * 60 * 1000, nullptr); // five minutes, then stop watching
    if (google) ShellExecuteW(nullptr, L"open", sys::cal::Calendars::GoogleSettings(), nullptr, nullptr, SW_SHOWNORMAL);
    OnClipboard(); // it may already be copied
}

void Model::StopCalWait() {
    if (!calWaiting) return;
    RemoveClipboardFormatListener(notify);
    KillTimer(notify, kCalWaitTimer);
    calWaiting = false;
}

bool Model::OnClipboard() {
    if (!calWaiting || !IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    constexpr int kClipTries = 10; // the copying app, clipboard history or a manager may hold it open just as it tells us
    bool open = false;
    for (int i = 0; i < kClipTries && !(open = OpenClipboard(notify) != FALSE); ++i) Sleep(20);
    if (!open) return false;
    std::wstring text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text.assign(p, wcsnlen(p, 4096));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (!sys::cal::Calendars::LooksLikeCalendar(text) || !cals.Add(text, notify, WM_APP_CAL)) return false; // anything else copied meanwhile is left alone
    StopCalWait();
    calLinks = false;
    return true;
}

void Model::CloseSettings() {
    settingsOpen = false;
    setTab = -1; // back to whichever applies
    hexFocus = false;
}

void Model::SetNotifyPills(bool on) {
    SetPref(notifyPills, L"NotifyPills", on ? 1 : 0);
    SyncNotify();
}

void Model::SetArtBack(bool on) { SetPref(artBack, kArtBackVal, on ? 1 : 0); } // halobar picks it up on its next look

void Model::SetQuiet(bool on) {
    SetPref(quiet, kQuietVal, on ? 1 : 0); // halobar sees it within a moment, it watches our key
    SyncNotify();
}

void Model::SyncNotify() {
    bool want = PillsWanted() && notify;
    if (want == watching) return; // only on a change: starting asks windows for access, not something to repeat every second
    watching = want;
    if (want) notifyWatch.Start(notify, WM_APP_NOTIFY);
    else notifyWatch.Stop(); // quiet or nobody wants pills: nothing listens
}

bool Model::PollShared() {
    if (ourWatch.Changed(kOurKey)) { // only after a write to our key
        quiet = sys::ReadNum(kQuietVal, 0) ? 1 : 0; // halobar may have flipped it
        artBack = sys::ReadNum(kArtBackVal, 0) ? 1 : 0; // and this one
        vizAlpha = (std::clamp)(sys::ReadNum(kVizAlphaVal, 60), kVizAlphaMin, 100); // and the two sliders
        vizGain = (std::clamp)(sys::ReadNum(kVizGainVal, 100), kVizGainMin, kVizGainMax);
    }
    bool runs = FindWindowW(kHaloClass, nullptr) != nullptr;
    bool theme = Following() || settingsOpen; // the settings swatch previews it too
    bool fresh = haloWatch.Changed(kHaloKey); // asked every time: it also re-arms the notice
    if (!fresh && runs == haloRuns && theme == haloThemeWas && setNow == haloSetWas) { // nothing it was read from has changed: the same answer as last second
        SyncNotify();
        return false;
    }
    haloRuns = runs;
    haloThemeWas = theme;
    haloSetWas = setNow;
    Halo was = halo, now;
    bool pills = haloRuns; // running without saved settings: its default is pills on
    int dock = -1; // its dock's edge, only while it runs
    int reach = 0; // its bottom dock's height off the screen's edge
    HKEY key = nullptr;
    if ((haloRuns || theme) && RegOpenKeyExW(HKEY_CURRENT_USER, kHaloKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        pills = haloRuns && HaloNum(key, L"Pills", 1, 0, 1) != 0;
        static constexpr int kHaloToOurs[] = {-1, 1, 2, 0}; // halobar counts bottom, left, right, top; we have no bottom
        int haloSide = HaloNum(key, L"Edge", 0, 0, 3);
        dock = haloRuns ? kHaloToOurs[haloSide] : -1;
        reach = haloRuns && haloSide == 0 ? HaloNum(key, L"DockReach", 0, 0, kMaxDockReach) : 0; // halobar writes it as its size changes
        if (theme) { // its theme matters only while we wear it or show it
            now.here = true;
            bool notDesk = setNow == 1 && HaloNum(key, L"AppsTheme", 0, 0, 1) != 0; // following it, each of our themes wears its match: its not-on-desktop one, if it has one
            auto themeNum = [&](const wchar_t* name, int fallback, int lo, int hi) {
                int v = HaloNum(key, name, fallback, lo, hi);
                return notDesk ? HaloNum(key, (std::wstring(name) + L"Apps").c_str(), v, lo, hi) : v; // never set there: its desktop value
            };
            now.look = themeNum(L"Look", -1, -1, kLookChrome);
            if (now.look == kLookHalo || now.look == kLookStarfall) now.look = -1; // numbers of ours it never writes
            now.glassTint = themeNum(L"GlassTint", -1, -1, 0xFFFFFF);
            now.accent = themeNum(L"Accent", -1, -1, 1);
            now.accentHex = themeNum(L"AccentHex", now.accentHex, INT_MIN, INT_MAX) & 0xFFFFFF;
            now.customBg = themeNum(L"CustomBg", now.customBg, INT_MIN, INT_MAX) & 0xFFFFFF;
            now.glassBlur = themeNum(L"GlassBlur", now.glassBlur, 0, 100);
            now.opacity = themeNum(L"Opacity", now.opacity, kMinOpacity, 100);
        }
        RegCloseKey(key);
    }
    haloPills = pills;
    haloEdge = dock; // the drawer re-docks off it once the panel is closed, and back when it leaves
    haloReach = reach; // the sidebar fits above its dock
    if (theme) halo = now; // not installed: its defaults, which follow us, so dark
    SyncNotify();
    return (Following() || settingsOpen) && halo != was; // open settings repaint too: the halobar swatch, and whether it's offered at all
}

void Model::ReadAccess() {
    constexpr int kWinTextMax = 225; // the most windows' own slider goes
    HIGHCONTRASTW hc = {sizeof(hc)};
    sysContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    DWORD pct = 100, size = sizeof(pct);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr, &pct, &size) != ERROR_SUCCESS) pct = 100; // missing: never changed
    sysText = (std::clamp)((int)pct, 100, kWinTextMax);
}

bool Model::HighContrast() const { return a11yContrast == kContrastOn || (a11yContrast == kContrastAuto && sysContrast); }

int Model::TextPct() const { return (std::max)(a11yText, a11yFollow ? sysText : 100); }

int Model::Look() const {
    if (HighContrast()) return 0; // no glass under it: everything solid
    if (!Following()) return look;
    return halo.look >= 0 ? halo.look : 0; // halobar follows us while we follow it: dark ends the loop
}

int Model::AccentMode() const { return Following() && halo.accent >= 0 ? halo.accent : accentMode; }
int Model::AccentHex() const {
    if (Following() && halo.accent == 1) return halo.accentHex;
    return accentMode == kAccentWindows && winAccentHex >= 0 ? winAccentHex : accentHex;
}
int Model::CustomBg() const { return Following() && halo.look == 3 ? halo.customBg : customBg; }
int Model::GlassBlur() const { return Following() && (halo.look == 2 || halo.look == kLookClear) ? halo.glassBlur : glassBlur; }
int Model::BgOpacity() const {
    if (!Following()) return bgOpacity;
    return halo.look >= 0 ? halo.opacity : kMaxOpacity; // halobar follows us back: the default ends the loop, as the look's dark does
}

void Model::SetAccent(int mode, int hex) {
    SetPref(accentMode, L"AccentMode", (std::clamp)(mode, 0, kAccentWindows));
    SetPref(accentHex, L"AccentHex", hex & 0xFFFFFF);
    if (accentMode == kAccentWindows) { // windows' own accent, and word when it changes
        winAccentHex = winAccent.Read();
        winAccent.Watch(notify, WM_APP_ACCENT);
    } else {
        winAccent.Stop(); // nothing listens unless it's picked
    }
}

bool Model::ParseHex(const std::wstring& text, int& rgb) {
    std::wstring h;
    for (wchar_t c : text)
        if (iswxdigit(c)) h += c;
        else if (c != L'#' && !iswspace(c)) return false; // anything else isn't a color
    if (h.size() == 3) h = {h[0], h[0], h[1], h[1], h[2], h[2]}; // #abc -> #aabbcc
    if (h.size() != 6) return false;
    rgb = (int)wcstoul(h.c_str(), nullptr, 16);
    return true;
}

bool Model::CommitHex() {
    hexFocus = false;
    int rgb = 0;
    if (!ParseHex(hexEdit, rgb)) return false;
    if (hexTarget == 1) SetPref(customBg, L"CustomBg", rgb & 0xFFFFFF);
    else SetAccent(1, rgb);
    return true;
}

void Model::SetLook(int v) {
    look = v < 0 || v >= kLooks ? 0 : v;
    sys::SaveNum(ThemeKey(kLookVal).c_str(), look);
    PollShared(); // following halobar: its theme now, not a second later
}

namespace {

using GetOverlayFn = DWORD(WINAPI*)(GUID*); // undocumented powrprof exports
using SetOverlayFn = DWORD(WINAPI*)(const GUID*);
const GUID kEco = {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}}; // best efficiency overlay
const GUID kPerf = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}}; // best performance overlay

FARPROC PowrProc(const char* name) {
    static HMODULE module = LoadLibraryW(L"powrprof.dll"); // loaded once, never freed
    return module ? GetProcAddress(module, name) : nullptr; // runtime lookup, missing on older builds
}

int ReadPower() {
    auto get = (GetOverlayFn)PowrProc("PowerGetEffectiveOverlayScheme");
    GUID g = {};
    if (!get || get(&g) != ERROR_SUCCESS) return -1;
    if (IsEqualGUID(g, kEco)) return 0;
    if (IsEqualGUID(g, kPerf)) return 2;
    return 1; // null guid = balanced
}

}

void Model::CyclePower() {
    auto set = (SetOverlayFn)PowrProc("PowerSetActiveOverlayScheme");
    if (!set || pwrMode < 0) return;
    const GUID balanced = {}; // balanced = empty overlay
    const GUID& next = pwrMode == 0 ? balanced : pwrMode == 1 ? kPerf : kEco; // eco -> balanced -> perf -> eco
    if (set(&next) == ERROR_SUCCESS) pwrMode = ReadPower(); // re-read, os may refuse
}

static void SaveFaces(const std::vector<int>& on, const std::map<int, int>& sizes) {
    std::wstring all;
    for (int k : on) {
        for (const char* p = faces::Info(k).key; *p; ++p) all += (wchar_t)*p; // keys are plain ascii
        if (auto it = sizes.find(k); it != sizes.end()) all += L':', all += (wchar_t)(L'0' + it->second); // never chosen: nothing, so it keeps following its default
        all += L',';
    }
    sys::SaveText(L"Widgets", all);
}

void Model::SetFaceSize(int kind, int size) {
    faceSize[kind] = (std::clamp)(size, 0, 2);
    SaveFaces(faceOn, faceSize);
}

void Model::PlaceFaces(const std::vector<int>& order) {
    faceOn = order;
    SaveFaces(faceOn, faceSize);
}

void Model::SetFaces(const std::vector<int>& on, const std::map<int, int>& sizes) { // the whole board at once: its editor's undo and reset
    faceOn = on;
    faceSize = sizes;
    SaveFaces(faceOn, faceSize);
    SyncFaces();
}

void Model::ToggleFace(int kind) {
    auto it = std::find(faceOn.begin(), faceOn.end(), kind);
    if (it != faceOn.end()) {
        faceOn.erase(it);
    } else {
        faceOn.push_back(kind);
        if (!HasPage(kPgWidgets)) { // the first one brings its page onto the rail
            std::vector<int> next = pages;
            next.push_back(kPgWidgets);
            SetPages(next);
        }
    }
    SaveFaces(faceOn, faceSize);
    SyncFaces();
}

void Model::OpenFace(int kind) {
    faces::OpenPanel(kind);
    facePanel = kind;
    faceKeys = faces::WantsKeys(kind);
    if (faceKeys && notify) SetForegroundWindow(notify);
}

void Model::CloseFace() {
    if (facePanel >= 0) faces::ClosePanel();
    facePanel = -1;
    faceKeys = false;
}

void Model::SyncFaces() {
    if (!notify) return;
    feeds::Start(notify, WM_APP_FEEDS, L"Astraeus");
    std::vector<std::string> ids = faces::Feeds(faceOn);
    if (!panelLive && !studioOpen) ids.erase(std::remove_if(ids.begin(), ids.end(), [](const std::string& id) { return id != "screen"; }), ids.end()); // nothing's in sight: only screen time needs to keep noting
    bool market = pillLeft == kWMarket || pillRight == kWMarket || (AnyLong() && std::find(deskItems.begin(), deskItems.end(), (int)kWMarket) != deskItems.end()); // a pill shows the market: it stays fetched while the panel's away
    if (market && std::find(ids.begin(), ids.end(), kMarketQuote) == ids.end()) ids.push_back(kMarketQuote);
    auto onPill = [&](int k) { return std::find(deskItems.begin(), deskItems.end(), k) != deskItems.end() || (deskSplit && std::find(deskItemsApps.begin(), deskItemsApps.end(), k) != deskItemsApps.end()); };
    if (AnyLong() && onPill(kWMarket) && std::find(ids.begin(), ids.end(), kMarketQuote) == ids.end()) ids.push_back(kMarketQuote);
    if (AnyLong() && onPill(kDScore)) // the long pill's score: its leagues stay fetched while the panel's away
        for (const std::string& l : sports::Followed())
            if (std::string id = sports::FeedId(l); std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    feeds::Want(ids);
    if (!wx.place.empty() && feeds::Setting("sky.place") != wx.place) feeds::SetSetting("sky.place", wx.place);
}

void Model::SetOutTo(const std::wstring& id) {
    if (id == outId || !sys::SetOut(id)) return;
    outId = id;
    spk.Refresh(); // rebind volume + meter to new default
    if (vizOn) meter.Refresh();
    ReadAudio();
}

void Model::CycleOut() {
    outs = sys::ListOuts(outId);
    if (outs.size() < 2) return;
    size_t current = 0;
    for (size_t i = 0; i < outs.size(); ++i)
        if (outs[i].id == outId) current = i;
    SetOutTo(outs[(current + 1) % outs.size()].id);
}

void Model::StartTimer(int seconds) {
    timerTotal = (ULONGLONG)seconds * 1000;
    timerEnd = GetTickCount64() + timerTotal; // monotonic, unaffected by clock changes
    timerOn = true;
    timerDone = false;
}

void Model::StopTimer() {
    timerOn = false;
}

bool Model::TimerDone() {
    bool f = timerDone; // read-and-clear so alert fires once
    timerDone = false;
    return f;
}

std::wstring Model::TimerName() const {
    ULONGLONG seconds = timerTotal / 1000;
    return seconds % 60 == 0 ? std::to_wstring(seconds / 60) + L" min" : fmt::Clock((double)seconds);
}

double Model::TimerLeft() const {
    if (!timerOn) return 0;
    ULONGLONG now = GetTickCount64();
    return now >= timerEnd ? 0 : (timerEnd - now) / 1000.0; // clamp, avoid unsigned underflow
}

double Model::TimerFrac() const {
    if (!timerOn || timerTotal == 0) return 0;
    return 1.0 - TimerLeft() * 1000.0 / (double)timerTotal; // elapsed fraction for ring
}

void Model::Bands(float (&out)[sys::Spectrum::kBands]) {
    spectrumAsked = GetTickCount64();
    if (!spectrum) spectrum = std::make_unique<sys::Spectrum>();
    spectrum->Read(out);
    if (!vizFull) return;
    static float peak = kQuietFloor; // the bands' recent loudest, slowly forgotten: quiet playback fills the range as loud does
    float m = 0;
    for (float v : out) m = (std::max)(m, v);
    peak = (std::max)({m, peak * kForget, kQuietFloor});
    for (float& v : out) v = (std::min)(1.0f, v / peak);
}

bool Model::Levels(float& left, float& right) {
    bool ok = meter.Read(left, right);
    if (vizFull) { // windows turned down, or the app is: the levels are measured against their own recent loudest instead
        static float peak = kQuietFloor;
        peak = (std::max)({left, right, peak * kForget, kQuietFloor});
        left = (std::min)(1.0f, left / peak * kFullHead), right = (std::min)(1.0f, right / peak * kFullHead);
    }
    float g = VizGain();
    left = (std::min)(1.0f, left * g), right = (std::min)(1.0f, right * g); // the sensitivity slider: louder or calmer, never past full
    return ok;
}

void Model::IdleSpectrum() {
    if (spectrum && GetTickCount64() - spectrumAsked > 3000) spectrum.reset(); // page changed, panel closed or stage left: capture stops
}

void Model::ReadBattery() {
    sys::BatteryInfo b = sys::ReadBattery(false); // one cheap call, on power events only
    batPct = b.pct;
    charging = b.pct >= 0 && b.charging; // plugged in and full, or held at a care limit, isn't charging
}

int Model::Activity() const {
    if (timerOn) return kActTimer;
    if (focusOn) return kActFocus;
    if (swOn) return kActWatch; // a paused stopwatch isn't running
    return kActNone;
}

std::wstring Model::ActivityText() const { // short enough for a 30 dip bubble: 4:59, then whole minutes past ten
    double secs = 0;
    switch (Activity()) {
    case kActTimer: secs = std::ceil(TimerLeft()); break;
    case kActFocus: secs = std::ceil(FocusLeft()); break;
    case kActWatch: secs = (double)(SwMs() / 1000); break;
    default: return L"";
    }
    long long s = (long long)secs;
    if (s >= 3600) return std::to_wstring(s / 3600) + L"h";
    if (s >= 600) return std::to_wstring(s / 60) + L"m";
    wchar_t buf[16];
    swprintf_s(buf, L"%lld:%02lld", s / 60, s % 60);
    return buf;
}

float Model::ActivityFrac() const {
    switch (Activity()) {
    case kActTimer: return (float)TimerFrac();
    case kActFocus: return (float)FocusFrac();
    case kActWatch: return (float)(SwMs() % 60000) / 60000.0f; // a lap of the ring a minute
    default: return 0;
    }
}

void Model::CommitCity() {
    cityFocus = false;
    std::wstring city = cityEdit;
    size_t a = city.find_first_not_of(L" \t"), b = city.find_last_not_of(L" \t");
    city = a == std::wstring::npos ? L"" : city.substr(a, b - a + 1);
    cityEdit.clear();
    if (city == weather.City()) return;
    weather.SetCity(city, notify, WM_APP_WEATHER); // empty: back to finding you by ip
}

constexpr size_t kPillWordMax = 10, kPillTitleMax = 18; // the pill has room for a short word beside its clock
constexpr size_t kDeskSongMax = 32; // the long pill has room for a song's name
constexpr double kLyricHold = 1.5; // seconds a line stays after it's sung
constexpr double kLyricBridge = 5; // a breath shorter than this keeps the line up till the next
constexpr double kLyricLone = 8; // a line with no end of its own: about this long

// the line being sung while a song plays, and the seconds until that changes (-1: nothing coming); hold keeps its lyrics alive while the line is read
static const sys::LyricLine* SungLine(const Model& m, std::shared_ptr<const sys::Lyrics>& hold, double& wait) {
    wait = -1;
    const sys::Track& tr = m.track;
    if (!m.hasTrack || !tr.playing || !tr.music || tr.title.empty()) return nullptr;
    hold = m.lyrics.Get(sys::LyricSrc::Key(tr.title, tr.artist));
    if (!hold || hold->lines.empty()) return nullptr;
    const std::vector<sys::LyricLine>& ls = hold->lines;
    double t = tr.Elapsed() / 1e7 - hold->offsetMs / 1000.0; // the song's own nudge, as fullscreen has it
    auto next = std::upper_bound(ls.begin(), ls.end(), t, [](double v, const sys::LyricLine& l) { return v < l.start; });
    if (next == ls.begin()) {
        wait = next->start - t;
        return nullptr;
    }
    const sys::LyricLine& at = *(next - 1);
    double until = (at.end > at.start ? at.end : at.start + kLyricLone) + kLyricHold;
    if (next != ls.end() && next->start - until < kLyricBridge) until = next->start;
    if (next != ls.end()) until = (std::min)(until, next->start);
    if (t < until && !at.text.empty()) {
        wait = until - t;
        return &at;
    }
    if (next != ls.end()) wait = next->start - t;
    return nullptr;
}

bool Model::PillLyrics() const {
    auto on = [](const std::vector<int>& v) { return std::find(v.begin(), v.end(), (int)kDLyrics) != v.end(); };
    return AnyLong() && (on(deskItems) || (deskSplit && on(deskItemsApps)));
}

double Model::LyricWait() const {
    std::shared_ptr<const sys::Lyrics> hold;
    double wait = -1;
    SungLine(*this, hold, wait);
    return wait;
}

std::vector<int> Model::DeskDefaults() { return {kWWeather, kDMusic, kDClock, kWCpu, kDVolume}; }

const wchar_t* Model::DeskItemName(int k) {
    switch (k) {
    case kDClock: return L"Clock";
    case kDDate: return L"Date";
    case kDVolume: return L"Volume";
    case kDWifi: return L"Wi-Fi";
    case kDMusic: return L"Music";
    case kDTimer: return L"Timer";
    case kDCover: return L"Cover";
    case kDBars: return L"Visualizer";
    case kWWeather: return L"Weather";
    case kWBattery: return L"Battery";
    case kWCpu: return L"CPU";
    case kWRam: return L"RAM";
    case kWGpu: return L"GPU";
    case kWNet: return L"Network";
    case kWWork: return L"Workspace";
    case kWLang: return L"Language";
    case kWTitle: return L"Window";
    case kWDisk: return L"Disk";
    case kWMarket: return L"Markets";
    case kDScore: return L"Score";
    case kDLyrics: return L"Lyrics";
    default: return L"";
    }
}

static_assert(std::size(glyph::kSportIcons) == sports::kSports, "a mark for every sport");

std::wstring Model::DeskItemText(int k, int& icon) const {
    icon = 0;
    SYSTEMTIME st;
    wchar_t buf[48] = {};
    switch (k) {
    case kDClock:
        GetLocalTime(&st);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 48);
        return buf;
    case kDDate:
        GetLocalTime(&st);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", buf, 48, nullptr);
        return buf;
    case kDVolume:
        icon = muted || vol < 0.005f ? glyph::VolumeOff : vol < 0.5f ? glyph::VolumeDown : glyph::VolumeUp;
        return muted ? L"" : std::to_wstring((int)std::lround(vol * 100)) + L"%";
    case kDWifi:
        icon = net.ssid.empty() ? glyph::WifiOff : glyph::Wifi; // the icon alone: a network's name stays off the desktop
        return L"";
    case kDMusic: {
        if (!hasTrack || track.title.empty()) return L"";
        icon = glyph::Music;
        return track.title.size() > kDeskSongMax ? track.title.substr(0, kDeskSongMax - 1) + L"…" : track.title;
    }
    case kDCover:
    case kDBars: // drawn by the pill itself; the icon only says there's a song for them
        if (hasTrack && !track.title.empty()) icon = glyph::Music;
        return L"";
    case kDScore: { // games on now, or over within kRecentFor; none: the latest results. what's next never shows. the wheel steps through them
        std::vector<sports::Game> on, past;
        double now = (double)std::time(nullptr);
        {
            std::lock_guard<std::mutex> hold(scoreLock);
            if (scoreSeen.size() > kSeenCap) scoreSeen.clear(); // a few leagues' games; only a long run gets here
            for (const std::string& l : sports::Followed()) {
                const sports::League* lg = sports::Find(l);
                int sport = lg ? lg->sport : sports::kFootball;
                double len = GameLen(sport);
                for (const std::wstring& row : feeds::Get(sports::FeedId(l)).rows) {
                    sports::Game g = sports::Decode(row);
                    g.sport = sport;
                    ScoreSeen& seen = scoreSeen[std::wstring(l.begin(), l.end()) + L"|" + g.a + L"|" + g.b + L"|" + std::to_wstring((long long)g.when)];
                    if (g.state == sports::kLive) {
                        seen.live = true;
                        on.push_back(std::move(g));
                        continue;
                    }
                    if (g.state != sports::kFinal) continue;
                    if (seen.live && seen.end == 0) seen.end = now; // watched it finish
                    double end = seen.end ? seen.end : g.when + len; // over before we looked: about its usual length after the start
                    (now - end < kRecentFor ? on : past).push_back(std::move(g));
                }
            }
        }
        std::vector<sports::Game> all = sports::Order(std::move(on)); // live first, then the newest finished; it opens on these
        for (sports::Game& g : sports::Order(std::move(past))) all.push_back(std::move(g)); // older results after them, for the wheel
        if (all.empty()) return L"";
        int n = (int)all.size();
        const sports::Game& shown = all[(size_t)(((scoreAt % n) + n) % n)];
        icon = shown.sport >= 0 && shown.sport < sports::kSports ? glyph::kSportIcons[shown.sport] : glyph::Trophy; // its own sport's mark
        return sports::Line(shown);
    }
    case kDLyrics: { // the sung line; with none, the pill's middle shows what it always does
        std::shared_ptr<const sys::Lyrics> hold;
        double wait = -1;
        const sys::LyricLine* line = SungLine(*this, hold, wait);
        return line ? line->text : L"";
    }
    case kDTimer:
        if (Activity() == kActNone) return L"";
        icon = glyph::Timer;
        return ActivityText();
    case kWNet: { // the long pill has the room for both ways
        auto rate = [](double bytes) {
            double k = bytes / 1024;
            return k < 1000 ? std::to_wstring((int)std::lround(k)) + L"K" : std::to_wstring((int)std::lround(k / 1024)) + L"M";
        };
        return L"↓ " + rate(downHist.Last()) + L"  ↑ " + rate(upHist.Last());
    }
    default:
        return PillWidget(k, icon);
    }
}

void Model::SetDeskItems(const std::vector<int>& items) {
    deskItems = items;
    std::wstring s;
    for (int v : items) s += (s.empty() ? L"" : L",") + std::to_wstring(v);
    sys::SaveText(L"DeskItems", s.empty() ? L"-" : s);
    SyncFaces(); // the market may have come on or gone off the long pill
}

void Model::SetDeskItemsApps(const std::vector<int>& items) {
    deskItemsApps = items;
    std::wstring s;
    for (int v : items) s += (s.empty() ? L"" : L",") + std::to_wstring(v);
    sys::SaveText(L"DeskItemsApps", s.empty() ? L"-" : s);
    SyncFaces();
}

void Model::WantDeskAudio() {
    spk.Refresh();
    ReadAudio();
    if (vizOn) meter.Refresh(); // bind the level meter to the current output, for its visualizer
}

std::wstring Model::PillWidget(int which, int& icon) const {
    switch (which) {
    case kWMarket: { // the s&p 500's day, as the market widgets read it
        feeds::Series q = feeds::Get(kMarketQuote);
        if (!q.ok) return L"";
        icon = q.change >= 0 ? glyph::Up : glyph::Down;
        wchar_t b[32];
        swprintf_s(b, L"S&P %+.1f%%", q.change * 100);
        return b;
    }
    case kWWeather:
        if (!cfg.weather || !wx.valid) return L"";
        icon = fmt::WxIcon(wx.code, wx.isDay);
        return fmt::Temp(wx.tempC, fahr != 0);
    case kWBattery:
        if (batPct < 0) return L"";
        icon = charging ? glyph::BatteryCharging : batPct <= batLow ? glyph::BatteryLow : glyph::Battery;
        return std::to_wstring(batPct) + L"%";
    case kWCpu:
        icon = glyph::Cpu;
        return std::to_wstring((int)std::lround(cpuHist.Last() * 100)) + L"%";
    case kWRam:
        icon = glyph::Ram;
        return std::to_wstring((int)std::lround(ramHist.Last() * 100)) + L"%";
    case kWGpu:
        if (stats.gpu < 0) return L""; // no counter on this pc
        icon = glyph::Gpu;
        return std::to_wstring((int)std::lround(stats.gpu * 100)) + L"%";
    case kWWork: {
        if (tiler == sys::Tiler::kNone || wsNow.current < 0 || wsNow.current >= (int)wsNow.list.size()) return L"";
        icon = glyph::Workspaces;
        const std::wstring& name = wsNow.list[wsNow.current].name;
        return name.size() > kPillWordMax ? name.substr(0, kPillWordMax - 1) + L"…" : name;
    }
    case kWLang:
        icon = glyph::Translate;
        return sys::extras::Language();
    case kWTitle: {
        std::wstring s = sys::extras::ActiveTitle();
        if (s.empty()) return L"";
        icon = glyph::Window;
        size_t cut = s.find(L" - "); // "page - app": the part that changes
        if (cut != std::wstring::npos && cut > 0) s = s.substr(0, cut);
        return s.size() > kPillTitleMax ? s.substr(0, kPillTitleMax - 1) + L"…" : s;
    }
    case kWDisk: {
        float used = 0;
        double free = 0;
        if (!sys::extras::SystemDrive(used, free)) return L"";
        icon = glyph::Disk;
        return std::to_wstring((int)std::lround(used * 100)) + L"%";
    }
    case kWNet: {
        icon = glyph::Down;
        double k = downHist.Last() / 1024; // short: the pill has room for four or five characters
        return k < 1000 ? std::to_wstring((int)std::lround(k)) + L"K" : std::to_wstring((int)std::lround(k / 1024)) + L"M";
    }
    default: return L"";
    }
}
