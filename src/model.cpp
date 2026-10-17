#include "model.h"

#include <iphlpapi.h>
#include <icmpapi.h>
#include "picker.h"

#include <algorithm>

namespace {

int ReadPower();

unsigned long long Ticks(const FILETIME& f) { // filetime -> 100ns count
    return (unsigned long long)f.dwHighDateTime << 32 | f.dwLowDateTime;
}

}

namespace {
constexpr wchar_t kLookVal[] = L"Appearance";
constexpr int kLooks = 4;
}

Model::Model(const Config& c) : cfg(c), portable(sys::IsPortable()), look(sys::ReadNum(kLookVal, 0)) {
    if (look < 0 || look >= kLooks) look = 0; // unknown value: back to dark
    glassBlur = (std::clamp)(sys::ReadNum(L"GlassBlur", 100), 0, 100);
    customBg = sys::ReadNum(L"CustomBg", customBg) & 0xFFFFFF;
    lyricOffsetMs = (std::clamp)(sys::ReadNum(L"LyricOffsetMs", 0), -3000, 3000);
    edge = (std::clamp)(sys::ReadNum(L"Edge", 0), 0, 2);
    screens = (std::clamp)(sys::ReadNum(L"Screens", 0), 0, 1);
    screen = sys::ReadText(L"Screen");
    islandSecs = (std::clamp)(sys::ReadNum(L"IslandSecs", 0), -1, kIslandMaxSecs);
    int iniHex = ((int)(cfg.themeCol.r * 255 + 0.5f) << 16) | ((int)(cfg.themeCol.g * 255 + 0.5f) << 8) | (int)(cfg.themeCol.b * 255 + 0.5f);
    accentMode = (std::clamp)(sys::ReadNum(L"AccentMode", cfg.wallTheme ? 0 : 1), 0, 1); // settings override the ini once used
    accentHex = sys::ReadNum(L"AccentHex", iniHex) & 0xFFFFFF;
    notifyPills = (std::clamp)(sys::ReadNum(L"NotifyPills", 0), 0, 1);
    fullLyrics = (std::clamp)(sys::ReadNum(L"FullLyrics", 1), 0, 1);
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0); // windows' own "animation effects": off means start calm
    reduceMotion = (std::clamp)(sys::ReadNum(L"ReduceMotion", anim ? 0 : 1), 0, 1);
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
    if (notifyPills) notifyWatch.Start(hwnd, WM_APP_NOTIFY);
    media.Start(hwnd, WM_APP_MEDIA);
    radios.Start(hwnd, WM_APP_RADIOS);
    Sample(0); // seed cpu/net baselines, pushes nothing
    worker = std::thread([this] { Worker(); });
}

void Model::SetLive(bool active, bool processes, bool ping) {
    shown = active; // ui thread: Tick samples every second only while shown
    if (!active) CloseMixer(); // the audio sessions and icons go with the panel
    {
        std::lock_guard<std::mutex> guard(lock);
        bool wakeNow = active && (!pollOn || (processes && !wantProcs)); // wake only if newly on or procs newly wanted
        pollOn = active;
        wantProcs = processes;
        wantPing = active && ping;
        if (!wakeNow) return; // otherwise next 1s cycle picks it up
    }
    wake.notify_all(); // notify outside lock, avoids instant re-block
}

void Model::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // stats never starve ui
    std::unique_lock<std::mutex> lk(lock);
    while (!quit) {
        wake.wait(lk, [this] { return quit || pollOn; }); // sleep while drawer closed
        if (quit) break;
        bool procsNow = wantProcs, pingNow = wantPing; // snapshot, flags may change while unlocked
        lk.unlock(); // slow sampling runs without lock
        sys::Stats s = statSrc.Sample();
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
    stats = newStats;
    if (wantProcs) procs = newProcs; // keep old list when procs not polled
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
    if (portable) { // a laptop's own battery: one cheap call, nothing held
        SYSTEM_POWER_STATUS ps{};
        bool have = GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryLifePercent <= 100;
        batPct = have ? ps.BatteryLifePercent : -1;
        charging = have && ps.ACLineStatus == 1;
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
    if (portable) pwrMode = ReadPower(); // laptop shows power mode, desktop shows outputs
    else outs = sys::ListOuts(outId);
    if (cfg.viz) meter.Refresh();
    bright.ReadAsync(notify, WM_APP_BRIGHT); // ddc/wmi is slow, result posted back
    if (cfg.weather) weather.Refresh(notify, WM_APP_WEATHER);
    netSrc.FetchWan(notify, WM_APP_WANIP); // async public ip lookup
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

void Model::OpenSettings() {
    if (settingsOpen) return;
    CloseBt(); // one sheet at a time
    CloseWifi();
    CloseCal();
    CloseMixer();
    settingsOpen = true;
    screenList = sys::ListScreens(); // monitors may have changed since last time
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

void Model::CloseSettings() {
    settingsOpen = false;
    hexFocus = false;
}

void Model::SetNotifyPills(bool on) {
    SetPref(notifyPills, L"NotifyPills", on ? 1 : 0);
    if (on) notifyWatch.Start(notify, WM_APP_NOTIFY);
    else notifyWatch.Stop();
}

void Model::SetAccent(int mode, int hex) {
    SetPref(accentMode, L"AccentMode", mode ? 1 : 0);
    SetPref(accentHex, L"AccentHex", hex & 0xFFFFFF);
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
    if (cfg.viz) meter.Refresh();
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
