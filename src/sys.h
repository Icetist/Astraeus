#pragma once
#include <windows.h>
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
    Net Sample();
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

bool CaptureOn(const RECT& monitor);

bool ReadFlag(const wchar_t* name, bool fallback);  // persisted setting, fallback if missing
void SaveFlag(const wchar_t* name, bool on);

void SetEco(bool on); // efficiency mode + low memory priority while idle
void SetAwake(bool on);  // keep display awake
void OpenSnip();
void OpenUri(const wchar_t* uri);

}
