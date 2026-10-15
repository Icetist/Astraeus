#include "share.h"

#include <audioclient.h>
#include <avrt.h>
#include <devicetopology.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

namespace sys {
namespace {

constexpr REFERENCE_TIME kCapBuf = 200000; // 20 ms loopback buffer, 100 ns units
constexpr REFERENCE_TIME kOutBuf = 1000000; // 100 ms per extra output: room for delay, fewer wakeups for the radio
constexpr double kCushionMs = 20; // minimum queue on an extra output, absorbs wake jitter
constexpr double kResyncMs = 150; // drift past this restarts the output instead of slewing
constexpr double kMaxFifoMs = 2000; // output stalled, stop buffering
constexpr ULONGLONG kSilenceMs = 3000; // extras stop after this much silence so bluetooth links can idle
constexpr float kSilent = 1e-5f; // peak below this counts as silence
constexpr double kMaxSkew = 0.003; // playback rate may move ±0.3% to hold sync, inaudible
constexpr double kSettleSec = 4; // drift error is worked off over about this long
constexpr DWORD kIdleWaitMs = 250; // wake period while nothing is playing
constexpr DWORD kPollMs = 10; // wake period when loopback events aren't supported
constexpr int kMaxDelayMs = 1000;
constexpr wchar_t kRegKey[] = L"Software\\Astraeus\\Share";
const PROPERTYKEY kNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14}; // PKEY_Device_FriendlyName

void Log(const wchar_t* what) { OutputDebugStringW((std::wstring(L"[share] ") + what + L"\n").c_str()); }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

int LoadInt(const std::wstring& id, const wchar_t* what, int fallback) {
    DWORD v = 0, size = sizeof(v);
    std::wstring name = id + L"|" + what;
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name.c_str(), RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? (int)v : fallback;
}

void SaveInt(const std::wstring& id, const wchar_t* what, int v) {
    std::wstring name = id + L"|" + what;
    DWORD d = (DWORD)v;
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name.c_str(), REG_DWORD, &d, sizeof(d));
}

struct Kind {
    bool bt = false, handsFree = false;
};

Kind Probe(IMMDevice* dev) { // the driver behind an endpoint tells bluetooth and its call profile apart, in any language
    Kind k;
    ComPtr<IDeviceTopology> topo, back;
    ComPtr<IConnector> pin, peer;
    ComPtr<IPart> part;
    LPWSTR raw = nullptr;
    if (FAILED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, (void**)topo.GetAddressOf())) || FAILED(topo->GetConnector(0, &pin)) ||
        FAILED(pin->GetConnectedTo(&peer)) || FAILED(peer.As(&part)) || FAILED(part->GetTopologyObject(&back)) || FAILED(back->GetDeviceId(&raw)))
        return k;
    std::wstring f = Lower(raw);
    CoTaskMemFree(raw);
    k.handsFree = f.find(L"bthhfenum") != std::wstring::npos; // hands-free profile: mono call quality, opening it drops music quality
    k.bt = k.handsFree || f.find(L"bthenum") != std::wstring::npos || f.find(L"bthledevice") != std::wstring::npos;
    return k;
}

bool IsFloat(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return f->wBitsPerSample == 32;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && f->wBitsPerSample == 32;
    return false;
}

class DefaultWatch : public IMMNotificationClient { // default output changed: the engine re-captures from the new one
public:
    explicit DefaultWatch(std::atomic<bool>* flag, HANDLE wake) : flag_(flag), wake_(wake) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) Kick();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        Kick(); // an extra may have dropped, reconcile
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void Kick() {
        flag_->store(true);
        SetEvent(wake_);
    }
    std::atomic<ULONG> ref_{1};
    std::atomic<bool>* flag_;
    HANDLE wake_;
};

struct Capture { // loopback of the default output, i.e. exactly what the main device plays
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;
    WAVEFORMATEX* fmt = nullptr;
    HANDLE ev = nullptr;
    bool evented = false;
    std::wstring id;
    UINT ch = 0, rate = 0;
    double latencyMs = 0; // main device's engine latency, extras are aligned to it

    ~Capture() {
        if (client) client->Stop();
        if (fmt) CoTaskMemFree(fmt);
        if (ev) CloseHandle(ev);
    }

    bool Open(IMMDeviceEnumerator* en) {
        ComPtr<IMMDevice> dev;
        LPWSTR raw = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || FAILED(dev->GetId(&raw))) return false;
        id = raw;
        CoTaskMemFree(raw);
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf())) || FAILED(client->GetMixFormat(&fmt)))
            return false;
        if (!IsFloat(fmt)) {
            Log(L"mix format isn't float, sharing unavailable");
            return false;
        }
        ch = fmt->nChannels;
        rate = fmt->nSamplesPerSec;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kCapBuf, 0, fmt, nullptr);
        evented = SUCCEEDED(hr) && SUCCEEDED(client->SetEventHandle(ev));
        if (!evented) { // older builds: loopback without events, poll instead
            client.Reset();
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf())) ||
                FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kCapBuf, 0, fmt, nullptr)))
                return false;
        }
        ComPtr<IAudioClient> probe; // an idle render client on the main device just to read its latency
        REFERENCE_TIME lat = 0;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)probe.GetAddressOf())) &&
            SUCCEEDED(probe->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kOutBuf, 0, fmt, nullptr)) && SUCCEEDED(probe->GetStreamLatency(&lat)))
            latencyMs = lat / 10000.0;
        return SUCCEEDED(client->GetService(IID_PPV_ARGS(&cap))) && SUCCEEDED(client->Start());
    }
};

struct Out { // one extra device
    std::wstring id;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<ISimpleAudioVolume> vol;
    UINT32 buf = 0; // render buffer, frames
    double latencyMs = 0;
    std::vector<float> fifo; // interleaved frames waiting to be written
    size_t rd = 0; // read position in fifo, frames
    double pos = 0; // fractional read offset for the resampler
    double target = 0; // frames that should be queued (fifo + device buffer)
    bool running = false, failed = false;
    int delayMs = -1; // -1 forces the first target computation
    float volume = -1;
};

}

struct Share::Impl {
    struct Want {
        bool on = false;
        int delayMs = 0;
        float volume = 1;
    };
    struct Seen {
        bool live = false, failed = false;
    };

    mutable std::mutex mu; // guards want + seen + hwnd
    std::map<std::wstring, Want> want; // ui intent per endpoint id
    std::map<std::wstring, Seen> seen; // engine report per endpoint id
    HWND hwnd = nullptr;
    UINT msg = 0;

    std::thread th;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> quit{false}, dirty{false}, restart{false};

    ~Impl() {
        Stop();
        if (wake) CloseHandle(wake);
    }

    Want& WantFor(const std::wstring& id) { // caller holds mu
        auto it = want.find(id);
        if (it != want.end()) return it->second;
        Want w;
        w.delayMs = std::clamp(LoadInt(id, L"delay", 0), 0, kMaxDelayMs);
        w.volume = std::clamp(LoadInt(id, L"vol", 1000) / 1000.0f, 0.0f, 1.0f);
        return want.emplace(id, w).first->second;
    }

    bool AnyOn() const { // caller holds mu
        for (const auto& [id, w] : want)
            if (w.on) return true;
        return false;
    }

    void Ping() {
        HWND h;
        UINT m;
        {
            std::lock_guard lock(mu);
            h = hwnd;
            m = msg;
        }
        if (h && IsWindow(h)) PostMessageW(h, m, 0, 0);
    }

    void Report(const std::wstring& id, bool live, bool failed) {
        bool changed = false;
        {
            std::lock_guard lock(mu);
            Seen& s = seen[id];
            changed = s.live != live || s.failed != failed;
            s.live = live;
            s.failed = failed;
        }
        if (changed) Ping();
    }

    void Start() {
        if (th.joinable()) return;
        quit = false;
        dirty = true;
        th = std::thread([this] { Run(); });
    }

    void Stop() {
        if (!th.joinable()) return;
        quit = true;
        SetEvent(wake);
        th.join(); // engine only stops clients and frees, returns quickly
        std::lock_guard lock(mu);
        seen.clear();
    }

    // ---- engine thread ----

    void Run() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Audio", &task); // glitch-free scheduling class for audio threads
        ComPtr<IMMDeviceEnumerator> en;
        DefaultWatch* watch = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
            watch = new DefaultWatch(&restart, wake);
            if (FAILED(en->RegisterEndpointNotificationCallback(watch))) {
                watch->Release();
                watch = nullptr;
            }
        }
        while (en && !quit) {
            restart = false;
            Session(en.Get());
            if (!quit && !restart) WaitForSingleObject(wake, 1000); // device gone or busy, retry shortly
        }
        if (watch) {
            en->UnregisterEndpointNotificationCallback(watch);
            watch->Release();
        }
        en.Reset();
        if (mm) AvRevertMmThreadCharacteristics(mm);
        if (SUCCEEDED(co)) CoUninitialize();
    }

    void Session(IMMDeviceEnumerator* en) { // one capture of the current default output, until it changes
        Capture cap;
        if (!cap.Open(en)) return;
        std::map<std::wstring, Out> outs;
        std::vector<float> chunk;
        ULONGLONG lastSound = GetTickCount64();
        dirty = true;
        while (!quit && !restart) {
            if (dirty.exchange(false)) Reconcile(en, cap, outs);
            bool anyLive = false;
            for (auto& [id, o] : outs) anyLive = anyLive || o.running;
            HANDLE waits[2] = {cap.ev, wake};
            if (cap.evented) WaitForMultipleObjects(2, waits, FALSE, anyLive ? 100 : kIdleWaitMs);
            else WaitForSingleObject(wake, kPollMs);
            if (quit || restart) break;
            float peak = 0;
            if (!Drain(cap, chunk, peak)) {
                restart = true; // main device invalidated
                break;
            }
            ULONGLONG now = GetTickCount64();
            if (peak > kSilent) lastSound = now;
            bool silent = now - lastSound > kSilenceMs; // also true when nothing plays and no packets arrive
            for (auto& [id, o] : outs) {
                if (o.failed) continue;
                if (silent) {
                    if (o.running) Halt(o);
                    continue;
                }
                if (!o.running && !Begin(cap, o)) continue;
                Push(cap, o, chunk);
                Feed(cap, o);
            }
        }
        for (auto& [id, o] : outs)
            if (o.running) Halt(o);
        std::lock_guard lock(mu);
        seen.clear(); // next session reports afresh
    }

    void Reconcile(IMMDeviceEnumerator* en, const Capture& cap, std::map<std::wstring, Out>& outs) {
        std::map<std::wstring, Want> w;
        {
            std::lock_guard lock(mu);
            w = want;
        }
        for (auto it = outs.begin(); it != outs.end();) { // close what's no longer wanted, or became the main output
            auto f = w.find(it->first);
            if (f == w.end() || !f->second.on || it->first == cap.id) {
                if (it->second.running) Halt(it->second);
                Report(it->first, false, false);
                it = outs.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [id, x] : w) {
            if (!x.on || id == cap.id) continue;
            auto [it, fresh] = outs.try_emplace(id);
            Out& o = it->second;
            if (fresh) {
                o.id = id;
                if (!OpenOut(en, cap, o)) {
                    o.failed = true;
                    Report(id, false, true);
                    continue;
                }
            }
            if (o.failed) continue;
            if (o.volume != x.volume) {
                o.volume = x.volume;
                if (o.vol) o.vol->SetMasterVolume(o.volume, nullptr); // the audio engine applies it, no work for us
            }
            if (o.delayMs != x.delayMs) {
                o.delayMs = x.delayMs;
                double align = (std::max)(0.0, cap.latencyMs - o.latencyMs); // device reports playing sooner: hold it back
                o.target = (kCushionMs + align + o.delayMs) * cap.rate / 1000.0;
                if (o.running) Halt(o); // restart prefilled to the new delay, no slow slewing
            }
        }
    }

    bool OpenOut(IMMDeviceEnumerator* en, const Capture& cap, Out& o) {
        ComPtr<IMMDevice> dev;
        REFERENCE_TIME lat = 0;
        if (FAILED(en->GetDevice(o.id.c_str(), &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)o.client.GetAddressOf())) ||
            FAILED(o.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, kOutBuf, 0,
                                        cap.fmt, nullptr)) || // engine converts rate/channels to the device's own mix, no extra pass here
            FAILED(o.client->GetBufferSize(&o.buf)) || FAILED(o.client->GetService(IID_PPV_ARGS(&o.render))))
            return false;
        o.client->GetService(IID_PPV_ARGS(&o.vol));
        if (SUCCEEDED(o.client->GetStreamLatency(&lat))) o.latencyMs = lat / 10000.0;
        return true;
    }

    bool Begin(const Capture& cap, Out& o) { // prefill silence up to the target so playback starts already aligned
        o.rd = 0;
        o.pos = 0;
        UINT32 pre = (UINT32)(std::min)((double)o.buf, o.target);
        BYTE* data = nullptr;
        if (pre && SUCCEEDED(o.render->GetBuffer(pre, &data))) o.render->ReleaseBuffer(pre, AUDCLNT_BUFFERFLAGS_SILENT);
        size_t rest = o.target > pre ? (size_t)(o.target - pre) : 0; // delay longer than the device buffer waits in our queue instead
        o.fifo.assign(rest * cap.ch, 0.0f);
        if (FAILED(o.client->Start())) {
            Lose(o);
            return false;
        }
        o.running = true;
        Report(o.id, true, false);
        return true;
    }

    void Halt(Out& o) {
        o.client->Stop();
        o.client->Reset(); // drop queued audio so a restart starts clean
        o.running = false;
        o.fifo.clear();
        o.rd = 0;
        Report(o.id, false, false);
    }

    void Lose(Out& o) {
        if (o.running) o.client->Stop();
        o.running = false;
        o.failed = true;
        Report(o.id, false, true);
    }

    bool Drain(Capture& cap, std::vector<float>& chunk, float& peak) {
        chunk.clear();
        UINT32 next = 0;
        HRESULT hr;
        while (SUCCEEDED(hr = cap.cap->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(hr = cap.cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            size_t at = chunk.size(), n = (size_t)frames * cap.ch;
            chunk.resize(at + n);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(chunk.begin() + at, chunk.end(), 0.0f);
            } else {
                const float* src = reinterpret_cast<const float*>(data);
                for (size_t i = 0; i < n; ++i) {
                    chunk[at + i] = src[i];
                    peak = (std::max)(peak, std::fabs(src[i]));
                }
            }
            cap.cap->ReleaseBuffer(frames);
        }
        return hr != AUDCLNT_E_DEVICE_INVALIDATED;
    }

    void Push(const Capture& cap, Out& o, const std::vector<float>& chunk) {
        if (chunk.empty()) return;
        if (o.rd > 4096) { // compact now and then instead of erasing per packet
            o.fifo.erase(o.fifo.begin(), o.fifo.begin() + o.rd * cap.ch);
            o.rd = 0;
        }
        o.fifo.insert(o.fifo.end(), chunk.begin(), chunk.end());
        if (o.fifo.size() / cap.ch - o.rd > kMaxFifoMs * cap.rate / 1000) Halt(o); // device stopped pulling, start over
    }

    void Feed(const Capture& cap, Out& o) {
        if (!o.running) return;
        UINT32 pad = 0;
        if (FAILED(o.client->GetCurrentPadding(&pad))) {
            Lose(o); // unplugged, out of range, or taken exclusive
            return;
        }
        const UINT ch = cap.ch;
        double have = (double)(o.fifo.size() / ch - o.rd) - o.pos;
        double err = have + pad - o.target; // frames queued beyond the target delay
        if (std::fabs(err) > kResyncMs * cap.rate / 1000) { // glitch or stall: realign from scratch
            Halt(o);
            return;
        }
        double ratio = 1 + std::clamp(err / (cap.rate * kSettleSec), -kMaxSkew, kMaxSkew); // run a hair fast or slow to hold sync
        UINT32 room = o.buf - pad;
        double span = have - 1; // last frame needs a neighbour to interpolate with
        UINT32 n = span < 0 ? 0 : (UINT32)(std::min)((double)room, std::floor(span / ratio) + 1);
        if (!n) return;
        BYTE* data = nullptr;
        if (FAILED(o.render->GetBuffer(n, &data))) return;
        float* dst = reinterpret_cast<float*>(data);
        const float* src = o.fifo.data() + o.rd * ch;
        double p = o.pos;
        for (UINT32 j = 0; j < n; ++j, p += ratio) { // linear interpolation, plenty for a ±0.3% rate change
            size_t i = (size_t)p;
            float f = (float)(p - i);
            const float* a = src + i * ch;
            for (UINT c = 0; c < ch; ++c) dst[j * ch + c] = a[c] + (a[ch + c] - a[c]) * f;
        }
        o.render->ReleaseBuffer(n, 0);
        size_t used = (size_t)p;
        o.rd += used;
        o.pos = p - used;
    }
};

Share::Share() : impl(std::make_unique<Impl>()) {}
Share::~Share() = default;

void Share::Notify(HWND hwnd, UINT msg) {
    std::lock_guard lock(impl->mu);
    impl->hwnd = hwnd;
    impl->msg = msg;
}

std::vector<ShareOut> Share::List() const {
    std::vector<ShareOut> out;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> all;
    ComPtr<IMMDevice> def;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)))
        return out;
    std::wstring defId;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && SUCCEEDED(def->GetId(&raw))) {
        defId = raw;
        CoTaskMemFree(raw);
    }
    UINT n = 0;
    all->GetCount(&n);
    std::lock_guard lock(impl->mu);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(all->Item(i, &dev)) || FAILED(dev->GetId(&raw))) continue;
        ShareOut s;
        s.id = raw;
        CoTaskMemFree(raw);
        Kind k = Probe(dev.Get());
        if (k.handsFree && s.id != defId) continue; // call profile: playing to it would drop the headset to phone quality
        s.bt = k.bt;
        ComPtr<IPropertyStore> props;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(kNameKey, &name)) && name.vt == VT_LPWSTR) {
            std::wstring full = name.pwszVal;
            size_t paren = full.find(L" (");
            bool split = paren != std::wstring::npos && paren > 0 && full.back() == L')';
            if (!split) s.name = full;
            else if (k.bt) s.name = full.substr(paren + 2, full.size() - paren - 3); // "Headphones (TWS V5.3)" -> the device's own name
            else s.name = full.substr(0, paren); // "Speakers (Realtek Audio)" -> "Speakers"
            std::wstring l = Lower(full);
            s.phones = l.find(L"headphone") != std::wstring::npos || l.find(L"headset") != std::wstring::npos || l.find(L"bud") != std::wstring::npos;
        }
        PropVariantClear(&name);
        if (s.name.empty()) s.name = L"Output " + std::to_wstring(i + 1);
        s.main = s.id == defId;
        const Impl::Want& w = impl->WantFor(s.id);
        s.on = w.on && !s.main;
        s.delayMs = w.delayMs;
        s.volume = w.volume;
        auto it = impl->seen.find(s.id);
        if (it != impl->seen.end()) {
            s.live = it->second.live;
            s.failed = it->second.failed;
        }
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const ShareOut& a, const ShareOut& b) { return a.main > b.main; }); // main output first
    return out;
}

void Share::Enable(const std::wstring& id, bool on) {
    bool any;
    {
        std::lock_guard lock(impl->mu);
        impl->WantFor(id).on = on;
        if (!on) impl->seen.erase(id);
        any = impl->AnyOn();
    }
    if (any) {
        impl->Start();
        impl->dirty = true;
        SetEvent(impl->wake);
    } else {
        impl->Stop(); // nothing shared: no thread, nothing held
    }
}

void Share::SetDelay(const std::wstring& id, int ms) {
    ms = std::clamp(ms, 0, kMaxDelayMs);
    {
        std::lock_guard lock(impl->mu);
        impl->WantFor(id).delayMs = ms;
    }
    SaveInt(id, L"delay", ms);
    impl->dirty = true;
    SetEvent(impl->wake);
}

void Share::SetVolume(const std::wstring& id, float v, bool persist) {
    v = std::clamp(v, 0.0f, 1.0f);
    {
        std::lock_guard lock(impl->mu);
        impl->WantFor(id).volume = v;
    }
    if (persist) SaveInt(id, L"vol", (int)std::lround(v * 1000));
    impl->dirty = true;
    SetEvent(impl->wake);
}

bool Share::Active() const {
    std::lock_guard lock(impl->mu);
    return impl->AnyOn();
}

void Share::StopAll() {
    {
        std::lock_guard lock(impl->mu);
        for (auto& [id, w] : impl->want) w.on = false;
    }
    impl->Stop();
}

}
