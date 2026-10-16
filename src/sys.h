#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct Stats {
    double cpu = 0;  // 0..1
    double mem = 0;  // used fraction 0..1
    double memUsedGb = 0, memTotalGb = 0;
    double gpu = -1;  // -1 = no gpu counter
    int threads = 0;  // logical cpus
};

class StatSrc {  // pimpl keeps pdh headers out of here
public:
    StatSrc();
    ~StatSrc();
    StatSrc(const StatSrc&) = delete;
    StatSrc& operator=(const StatSrc&) = delete;
    Stats Sample();  // delta since last call, call on a timer
private:
    struct Impl;
    std::unique_ptr<Impl> impl;  // null if init failed
};

struct Net {
    std::wstring adapter;
    std::wstring ssid;  // empty unless on wifi
    int signal = -1;  // wifi quality 0-100, -1 = n/a
    std::wstring lanIp;
    std::wstring wanIp;  // filled async by FetchWan
    double upBps = 0, downBps = 0;  // bytes per sec
    bool online = false;
};

class NetSrc {
public:
    NetSrc();
    ~NetSrc();
    NetSrc(const NetSrc&) = delete;
    NetSrc& operator=(const NetSrc&) = delete;
    Net Sample(bool refreshInfo = true);  // false: reuse last nic/ssid, skips the wlan service call
    void FetchWan(HWND notify, UINT msg);  // background lookup, posts msg when done
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct ProcRow {
    std::wstring name;  // no .exe, same-name processes merged
    double cpu = 0;  // share of all cores 0..1
    uint64_t memBytes = 0;  // private working set
};

class ProcSrc {
public:
    ProcSrc();
    ~ProcSrc();
    ProcSrc(const ProcSrc&) = delete;
    ProcSrc& operator=(const ProcSrc&) = delete;
    std::vector<ProcRow> Top(size_t n);  // first call has zero cpu, needs baseline
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class AudioDev {
public:
    static AudioDev Spk();  // default render endpoint
    static AudioDev Micro();  // default capture endpoint
    AudioDev(AudioDev&&) noexcept;
    AudioDev& operator=(AudioDev&&) noexcept;
    ~AudioDev();
    bool Refresh();  // rebind if default device changed
    bool Ready() const;
    float Level() const;  // scalar 0..1
    void SetLevel(float level);
    bool Muted() const;
    void SetMuted(bool muted);
private:
    explicit AudioDev(int flow);  // EDataFlow as int, avoids mmdeviceapi include
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct OutDev {
    std::wstring id;  // endpoint id string
    std::wstring name;
    bool headphones = false;
};

std::vector<OutDev> ListOuts(std::wstring& defaultId);
bool SetOut(const std::wstring& id);

bool IsPortable();

class Meter {
public:
    Meter();
    ~Meter();
    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;
    bool Refresh();
    bool Read(float& left, float& right);  // peak per channel 0..1
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class Bright {  // wmi brightness on worker thread
public:
    Bright();
    ~Bright();
    Bright(const Bright&) = delete;
    Bright& operator=(const Bright&) = delete;
    void ReadAsync(HWND notify, UINT msg);  // posts msg with percent in wparam, -1 if unknown
    void Set(int percent);  // non-blocking, latest value wins
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct Wx {
    bool valid = false;
    double tempC = 0, highC = 0, lowC = 0;
    int code = 0;  // wmo weather code
    bool isDay = true;
    std::wstring place;
};

class WxSvc {
public:
    WxSvc();
    ~WxSvc();
    WxSvc(const WxSvc&) = delete;
    WxSvc& operator=(const WxSvc&) = delete;
    void Refresh(HWND notify, UINT msg);  // no-op while cached or busy
    Wx Get() const;  // thread-safe snapshot
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

namespace power {
void Lock();
void Sleep();
void Restart();
void Shutdown();
void SignOut();
}

namespace detail { // winhttp get shared by weather, wan ip, updater
bool HttpGet(const wchar_t* url, std::string& body, std::atomic<bool>& stop, std::atomic<void*>& slot,
             size_t maxBytes = 256 * 1024);
void HttpCancel(std::atomic<bool>& stop, std::atomic<void*>& slot);
}

class Updater { // checks github releases, swaps in a newer exe
public:
    Updater();
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    enum Result : WPARAM { kInstalled, kCurrent, kFailed, kDownloading }; // wparam of msg; downloading only for manual checks
    void Check(HWND notify, UINT msg, bool manual = false); // background; posts kInstalled once the new exe is in place, a manual check also posts the other outcomes
    std::wstring Applied() const; // tag that was installed, or being fetched
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
bool FinishUpdate(); // at launch: clean up, true right after an update

bool ReadFlag(const wchar_t* name, bool fallback); // persisted hkcu setting, fallback if missing
void SaveFlag(const wchar_t* name, bool on);
int ReadNum(const wchar_t* name, int fallback);
std::wstring ClipText(); // clipboard text, empty if none
void SaveNum(const wchar_t* name, int value);
std::wstring ReadText(const wchar_t* name); // empty if missing
void SaveText(const wchar_t* name, const std::wstring& value);

struct Screen {
    std::wstring name; // device name, e.g. \\.\DISPLAY2, how a pick is remembered
    RECT rc{};
    bool primary = false;
    int number = 0; // the number windows display settings shows
};
std::vector<Screen> ListScreens();
HMONITOR FindScreen(const std::wstring& name); // null if that screen isn't connected
float Brightness(const RECT& r); // average luminance 0..1 of what's on screen there, from one tiny downscaled copy

void SetEco(bool on); // efficiency mode + low memory priority while idle
void SetAwake(bool on);  // keep display awake
void OpenSnip();
void OpenUri(const wchar_t* uri);

}
