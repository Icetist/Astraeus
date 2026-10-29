#include "model.h"

#include <iphlpapi.h>
#include <icmpapi.h>
#include "glyphs.h"
#include "pages.h"
#include "picker.h"
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cmath>

namespace {

int ReadPower();

unsigned long long Ticks(const FILETIME& f) { // filetime -> 100ns count
    return (unsigned long long)f.dwHighDateTime << 32 | f.dwLowDateTime;
}

}

namespace {
constexpr wchar_t kLookVal[] = L"Appearance";
constexpr int kLooks = Model::kLookStarfall + 1; // dark, light, glass, custom, halobar's, starfall
constexpr wchar_t kQuietVal[] = L"Quiet"; // do not disturb: halobar reads and writes the same value
constexpr wchar_t kHaloKey[] = L"Software\\HaloBar"; // halobar's own settings, never written from here
constexpr wchar_t kHaloClass[] = L"HaloBar"; // its window: there means it's running

constexpr wchar_t kPagesVal[] = L"PageOrder", kTilesVal[] = L"ToolTiles"; // saved by name: a new page or tile later never shifts the old ones
const wchar_t* const kPageKeys[Model::kPageIds] = {L"home", L"media", L"system", L"network", L"tools", L"shelf"};
const wchar_t* const kTileKeys[Model::kToolTiles] = {L"screenshot", L"color", L"awake", L"notifications", L"dnd", L"saver", L"airplane", L"settings"};
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
    if (pages.empty()) pages = {kPgHome, kPgMedia, kPgSystem, kPgNetwork, kPgShelf, kPgTools}; // every page, the shelf above the tools
    std::wstring tl = sys::ReadText(kTilesVal);
    tiles = tl.empty() ? std::vector<int>{kTlSnip, kTlColor, kTlAwake, kTlDnd, kTlSettings} : ParseList(tl, kTileKeys, kToolTiles); // the tiles the tools page always had
    stageHide = flag(L"StageHide", 1);
    uiScale = (std::clamp)(sys::ReadNum(L"UiScale", 100), 80, 130);
    lyricScale = (std::clamp)(sys::ReadNum(L"LyricScale", 100), kLyricMin, kLyricMax);
    bgOpacity = (std::clamp)(sys::ReadNum(L"BgOpacity", kMaxOpacity), kMinOpacity, kMaxOpacity);
    pillAlways = flag(L"PillAlways", 0);
    pillDesk = flag(L"PillDesktop", 1); // on out of the box: the island stays in sight on an empty desktop
    pillLeft = (std::clamp)(sys::ReadNum(L"PillLeft", kWWeather), 0, kWidgets - 1);
    pillRight = (std::clamp)(sys::ReadNum(L"PillRight", kWCpu), 0, kWidgets - 1);
    mediaCompact = flag(L"MediaCompact", 0);
    pillMusic = flag(L"PillMusic", 1);
    showArt = flag(L"ShowArt", 1);
    vizOn = flag(L"Visualizer", cfg.viz ? 1 : 0); // the ini's choice until the switch in settings is used
    hud = flag(L"Hud", 0); // experimental: off unless asked for
}

Model::~Model() {
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
    Sample(0); // seed cpu/net baselines, pushes nothing
    worker = std::thread([this] { Worker(); });
}

void Model::SetLive(bool active, bool processes, bool ping) {
    shown = active; // ui thread: Tick samples every second only while shown
    if (!active) CloseMixer(); // the audio sessions and icons go with the panel
    {
        std::lock_guard<std::mutex> guard(lock);
        bool wakeNow = active && ((processes && !wantProcs) || (ping && !wantPing)); // wake only if the system or network page just came up
        pollOn = active;
        wantProcs = active && processes;
        wantPing = active && ping;
        if (!wakeNow) return; // otherwise next 1s cycle picks it up
    }
    wake.notify_all(); // notify outside lock, avoids instant re-block
}

void Model::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // stats never starve ui
    std::unique_lock<std::mutex> lk(lock);
    while (!quit) {
        wake.wait(lk, [this] { return quit || (pollOn && (wantProcs || wantPing)); }); // asleep unless the system page (gauges, processes) or the network page (latency) shows
        if (quit) break;
        bool procsNow = wantProcs, pingNow = wantPing; // snapshot, flags may change while unlocked
        lk.unlock(); // slow sampling runs without lock
        sys::Stats s = procsNow ? statSrc.Sample() : sys::Stats{}; // the gpu counter is costly: only where its gauge is
        std::vector<sys::ProcRow> top;
        if (procsNow) top = procSrc.Top(6); // top 6 procs, only on sys page
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
        if (procsNow) newProcs = std::move(top);
        ready = true;
        PostMessageW(notify, WM_APP_STATS, 0, 0); // ui thread pulls via OnStats
        wake.wait_for(lk, std::chrono::seconds(1), [this, procsNow] { return quit || (wantProcs && !procsNow); }); // 1s cadence, early wake if procs newly wanted
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
    if (mixerOpen) mixer.Refresh(); // apps come and go while it's shown
    if (fullLyrics && hasTrack && track.music && track.duration > 0) // the song's lyrics, fetched while it plays: ready long before fullscreen opens; a cache check once known
        lyrics.Want(track.title, track.artist, track.album, track.duration / 1e7);
    if (focusOn && GetTickCount64() >= focusEnd) { // a block ends: focus hands over to a break, a break to the next round
        focusEvent = focusBreak ? 2 : 1;
        if (focusBreak) ++focusRound;
        focusBreak = !focusBreak;
        focusLen = (ULONGLONG)(focusBreak ? breakMin : focusMin) * 60000;
        focusEnd = GetTickCount64() + focusLen;
    }
    constexpr int kClosedEvery = 5; // closed: sample every 5s, nobody is looking at the graphs
    if (++unsampled < (shown ? 1 : kClosedEvery)) return;
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
    net = netSrc.Sample(shown); // rates are averaged over the gap by the sampler's own clock
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
    netSrc.FetchWan(notify, WM_APP_WANIP); // async public ip lookup
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
    if (fullLyrics && hasTrack && track.music && track.duration > 0) lyrics.Want(track.title, track.artist, track.album, track.duration / 1e7); // ready before fullscreen opens; no-op once known
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
    sys::SaveNum(key, value);
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

void Model::SetTiles(const std::vector<int>& order) {
    tiles = order;
    sys::SaveText(kTilesVal, JoinList(tiles, kTileKeys));
}

const wchar_t* Model::PageName(int id) {
    static const wchar_t* const kNames[kPageIds] = {L"Home", L"Media", L"System", L"Network", L"Tools", L"Shelf"};
    return kNames[(std::clamp)(id, 0, kPageIds - 1)];
}

int Model::PageGlyph(int id) {
    static const int kGlyphs[kPageIds] = {glyph::Overview, glyph::Music, glyph::System, glyph::Network, glyph::Tools, glyph::Shelf};
    return kGlyphs[(std::clamp)(id, 0, kPageIds - 1)];
}

const wchar_t* Model::TileName(int id) {
    static const wchar_t* const kNames[kToolTiles] = {L"Screenshot", L"Color picker", L"Awake", L"Notifications", L"Do not disturb", L"Energy saver", L"Airplane", L"Settings"};
    return kNames[(std::clamp)(id, 0, kToolTiles - 1)];
}

int Model::TileGlyph(int id) {
    static const int kGlyphs[kToolTiles] = {glyph::Snip, glyph::Edit, glyph::Coffee, glyph::Bell, glyph::Dnd, glyph::Saver, glyph::Airplane, glyph::Settings};
    return kGlyphs[(std::clamp)(id, 0, kToolTiles - 1)];
}

void Model::OpenSettings() {
    if (settingsOpen) return;
    CloseBt(); // one sheet at a time
    CloseWifi();
    CloseCal();
    CloseMixer();
    settingsOpen = true;
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
    mixer.Open();
}

void Model::CloseMixer() {
    mixerOpen = false;
    mixer.Close(); // the sessions and icons go with it
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
    hexFocus = false;
}

void Model::SetNotifyPills(bool on) {
    SetPref(notifyPills, L"NotifyPills", on ? 1 : 0);
    SyncNotify();
}

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
    quiet = sys::ReadNum(kQuietVal, 0) ? 1 : 0; // halobar may have flipped it
    haloRuns = FindWindowW(kHaloClass, nullptr) != nullptr;
    Halo was = halo, now;
    bool pills = haloRuns; // running without saved settings: its default is pills on
    int dock = -1; // its dock's edge, only while it runs
    HKEY key = nullptr;
    bool theme = Following() || settingsOpen; // the settings swatch previews it too
    if ((haloRuns || theme) && RegOpenKeyExW(HKEY_CURRENT_USER, kHaloKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        pills = haloRuns && HaloNum(key, L"Pills", 1, 0, 1) != 0;
        static constexpr int kHaloToOurs[] = {-1, 1, 2, 0}; // halobar counts bottom, left, right, top; we have no bottom
        dock = haloRuns ? kHaloToOurs[HaloNum(key, L"Edge", 0, 0, 3)] : -1;
        if (theme) { // its theme matters only while we wear it or show it
            now.here = true;
            now.look = HaloNum(key, L"Look", -1, -1, 3);
            now.accent = HaloNum(key, L"Accent", -1, -1, 1);
            now.accentHex = HaloNum(key, L"AccentHex", now.accentHex, INT_MIN, INT_MAX) & 0xFFFFFF;
            now.customBg = HaloNum(key, L"CustomBg", now.customBg, INT_MIN, INT_MAX) & 0xFFFFFF;
            now.glassBlur = HaloNum(key, L"GlassBlur", now.glassBlur, 0, 100);
            now.opacity = HaloNum(key, L"Opacity", now.opacity, kMinOpacity, 100);
        }
        RegCloseKey(key);
    }
    haloPills = pills;
    haloEdge = dock; // the drawer re-docks off it once the panel is closed, and back when it leaves
    if (theme) halo = now; // not installed: its defaults, which follow us, so dark
    SyncNotify();
    return (Following() || settingsOpen) && halo != was; // open settings repaint too: the halobar swatch, and whether it's offered at all
}

int Model::Look() const {
    if (!Following()) return look;
    return halo.look >= 0 ? halo.look : 0; // halobar follows us while we follow it: dark ends the loop
}

int Model::AccentMode() const { return Following() && halo.accent >= 0 ? halo.accent : accentMode; }
int Model::AccentHex() const {
    if (Following() && halo.accent == 1) return halo.accentHex;
    return accentMode == kAccentWindows && winAccentHex >= 0 ? winAccentHex : accentHex;
}
int Model::CustomBg() const { return Following() && halo.look == 3 ? halo.customBg : customBg; }
int Model::GlassBlur() const { return Following() && halo.look == 2 ? halo.glassBlur : glassBlur; }
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
    sys::SaveNum(kLookVal, look);
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

void Model::CycleOut() {
    outs = sys::ListOuts(outId);
    if (outs.size() < 2) return;
    size_t current = 0;
    for (size_t i = 0; i < outs.size(); ++i)
        if (outs[i].id == outId) current = i;
    const sys::OutDev& next = outs[(current + 1) % outs.size()];
    if (!sys::SetOut(next.id)) return;
    outId = next.id;
    spk.Refresh(); // rebind volume + meter to new default
    if (vizOn) meter.Refresh();
    ReadAudio();
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

std::wstring Model::PillWidget(int which, int& icon) const {
    switch (which) {
    case kWWeather:
        if (!cfg.weather || !wx.valid) return L"";
        icon = fmt::WxIcon(wx.code, wx.isDay);
        return fmt::Temp(wx.tempC, cfg.fahr);
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
    case kWNet: {
        icon = glyph::Down;
        double k = downHist.Last() / 1024; // short: the pill has room for four or five characters
        return k < 1000 ? std::to_wstring((int)std::lround(k)) + L"K" : std::to_wstring((int)std::lround(k / 1024)) + L"M";
    }
    default: return L"";
    }
}
