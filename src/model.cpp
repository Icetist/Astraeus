#include "model.h"

#include <algorithm>

namespace {

int ReadPower();

unsigned long long Ticks(const FILETIME& f) { // filetime -> 100ns count
    return (unsigned long long)f.dwHighDateTime << 32 | f.dwLowDateTime;
}

}

namespace {
constexpr wchar_t kLookVal[] = L"Appearance";
constexpr int kLooks = 3;
}

Model::Model(const Config& c) : cfg(c), portable(sys::IsPortable()), look(sys::ReadNum(kLookVal, 0)) {
    if (look < 0 || look >= kLooks) look = 0; // unknown value: back to dark
    glassBlur = (std::clamp)(sys::ReadNum(L"GlassBlur", 100), 0, 100);
    edge = (std::clamp)(sys::ReadNum(L"Edge", 0), 0, 2);
    screens = (std::clamp)(sys::ReadNum(L"Screens", 0), 0, 1);
    screen = sys::ReadText(L"Screen");
    int iniHex = ((int)(cfg.themeCol.r * 255 + 0.5f) << 16) | ((int)(cfg.themeCol.g * 255 + 0.5f) << 8) | (int)(cfg.themeCol.b * 255 + 0.5f);
    accentMode = (std::clamp)(sys::ReadNum(L"AccentMode", cfg.wallTheme ? 0 : 1), 0, 1); // settings override the ini once used
    accentHex = sys::ReadNum(L"AccentHex", iniHex) & 0xFFFFFF;
    notifyPills = (std::clamp)(sys::ReadNum(L"NotifyPills", 0), 0, 1);
}

Model::~Model() {
    {
        std::lock_guard<std::mutex> guard(lock);
        quit = true; // set under lock so worker sees it
    }
    wake.notify_all(); // break worker out of wait
    if (worker.joinable()) worker.join();
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

void Model::SetLive(bool active, bool processes) {
    shown = active; // ui thread: Tick samples every second only while shown
    {
        std::lock_guard<std::mutex> guard(lock);
        bool wakeNow = active && (!pollOn || (processes && !wantProcs)); // wake only if newly on or procs newly wanted
        pollOn = active;
        wantProcs = processes;
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
        bool procsNow = wantProcs; // snapshot, flags may change while unlocked
        lk.unlock(); // slow sampling runs without lock
        sys::Stats s = statSrc.Sample();
        std::vector<sys::ProcRow> top;
        if (procsNow) top = procSrc.Top(6); // top 6 procs, only on sys page
        lk.lock();
        newStats = s;
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
}

void Model::Tick() {
    if (timerOn && GetTickCount64() >= timerEnd) { // timer checked at tick granularity
        timerOn = false;
        timerDone = true; // ui consumes via TimerDone()
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
    wifiOpen = true;
    wifi.Open(notify, WM_APP_WIFI);
    OnWifi();
}

void Model::CloseWifi() {
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
    settingsOpen = true;
    screenList = sys::ListScreens(); // monitors may have changed since last time
}

void Model::PickScreen(const std::wstring& name) {
    screen = name;
    sys::SaveText(L"Screen", name);
    SetPref(screens, L"Screens", 0);
}

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
    SetAccent(1, rgb);
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
