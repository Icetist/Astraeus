#include "model.h"

#include <algorithm>

namespace {

int ReadPower();

unsigned long long Ticks(const FILETIME& f) { // filetime -> 100ns count
    return (unsigned long long)f.dwHighDateTime << 32 | f.dwLowDateTime;
}

}

namespace {
constexpr wchar_t kRecShow[] = L"ShowWhileRecording";
}

Model::Model(const Config& c)
    : cfg(c), portable(sys::IsPortable()), recShow(sys::ReadFlag(kRecShow, true)) {} // default: visible in captures

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
    media.Start(hwnd, WM_APP_MEDIA);
    radios.Start(hwnd, WM_APP_RADIOS);
    Tick(); // seed cpu baseline before first real tick
    worker = std::thread([this] { Worker(); });
}

void Model::SetLive(bool active, bool processes) {
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
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        unsigned long long i = Ticks(idle), t = Ticks(kernel) + Ticks(user); // kernel time includes idle
        if (lastTotal != 0 && t > lastTotal) cpuHist.Push(1.0f - (float)(i - lastIdle) / (float)(t - lastTotal)); // busy = 1 - idle delta / total delta
        lastIdle = i;
        lastTotal = t;
    }
    MEMORYSTATUSEX m = {sizeof(m)}; // dwLength must be set first
    if (GlobalMemoryStatusEx(&m)) ramHist.Push(m.dwMemoryLoad / 100.0f); // percent -> 0..1
    net = netSrc.Sample();
    upHist.Push((float)net.upBps);
    downHist.Push((float)net.downBps);

    if (timerOn && GetTickCount64() >= timerEnd) { // timer checked at tick granularity
        timerOn = false;
        timerDone = true; // ui consumes via TimerDone()
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
    btOpen = true;
    bt.Open(notify, WM_APP_BT);
    OnBt();
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

void Model::SetRecShow(bool on) {
    recShow = on;
    sys::SaveFlag(kRecShow, on);
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
