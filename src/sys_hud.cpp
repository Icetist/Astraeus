#include "hud.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace sys {

namespace {

constexpr float kLevelScale = 1000; // the level travels in wparam as thousandths

struct VolumeSink final : IAudioEndpointVolumeCallback { // windows calls this on its own audio thread
    std::atomic<HWND> notify{nullptr};
    UINT msg = 0;
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (!n) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (HWND h = notify.load(); h && d) PostMessageW(h, msg, (WPARAM)std::lround(d->fMasterVolume * kLevelScale), d->bMuted ? 1 : 0); // posted: the ui thread draws it
        return S_OK;
    }
};

}

struct VolumeWatch::Impl {
    ComPtr<IAudioEndpointVolume> vol;
    VolumeSink* sink = nullptr;
};

VolumeWatch::VolumeWatch() : impl(std::make_unique<Impl>()) {}
VolumeWatch::~VolumeWatch() { Stop(); }
bool VolumeWatch::On() const { return impl->sink != nullptr; }

bool VolumeWatch::Start(HWND notify, UINT msg) {
    Stop();
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(impl->vol.GetAddressOf()))))
        return false; // no speakers right now
    impl->sink = new VolumeSink;
    impl->sink->notify = notify;
    impl->sink->msg = msg;
    if (FAILED(impl->vol->RegisterControlChangeNotify(impl->sink))) {
        Stop();
        return false;
    }
    return true;
}

void VolumeWatch::Stop() {
    if (impl->sink) {
        impl->sink->notify = nullptr; // a call already on its way posts nothing
        if (impl->vol) impl->vol->UnregisterControlChangeNotify(impl->sink);
        impl->sink->Release();
        impl->sink = nullptr;
    }
    impl->vol.Reset();
}

// ---- brightness: WmiMonitorBrightnessEvent on its own mta thread, which sleeps on an event until stopped ----

namespace {

struct BrightSink final : IWbemObjectSink { // wmi calls this from its own threads
    std::atomic<HWND> notify{nullptr};
    UINT msg = 0;
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
            *out = static_cast<IWbemObjectSink*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (!n) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE Indicate(LONG n, IWbemClassObject** objs) override {
        for (LONG i = 0; i < n; ++i) {
            VARIANT v;
            VariantInit(&v);
            if (objs[i] && SUCCEEDED(objs[i]->Get(L"Brightness", 0, &v, nullptr, nullptr))) {
                int pct = v.vt == VT_UI1 ? v.bVal : v.vt == VT_I4 ? (int)v.lVal : v.vt == VT_UI4 ? (int)v.ulVal : -1; // uint8, whatever vt it arrives as
                if (HWND h = notify.load(); h && pct >= 0) PostMessageW(h, msg, (WPARAM)pct, 0);
            }
            VariantClear(&v);
        }
        return WBEM_S_NO_ERROR;
    }
    HRESULT STDMETHODCALLTYPE SetStatus(LONG, HRESULT, BSTR, IWbemClassObject*) override { return WBEM_S_NO_ERROR; }
};

}

struct BrightWatch::Impl { // shared with its thread: stopping never waits on a slow wmi call
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Impl() {
        if (stop) CloseHandle(stop);
    }
};

BrightWatch::BrightWatch() = default;
BrightWatch::~BrightWatch() { Stop(); }

void BrightWatch::Start(HWND notify, UINT msg) {
    Stop();
    auto state = std::make_shared<Impl>();
    if (!state->stop) return;
    impl = state;
    std::thread([state, notify, msg] {
        HANDLE stop = state->stop;
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // mta: wmi's callbacks need no pump
        ComPtr<IWbemLocator> loc;
        ComPtr<IWbemServices> svc;
        BrightSink* sink = nullptr;
        BSTR ns = SysAllocString(L"ROOT\\WMI"), lang = SysAllocString(L"WQL"), query = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessEvent");
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc))) && ns && lang && query &&
            SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) {
            CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            sink = new BrightSink;
            sink->notify = notify;
            sink->msg = msg;
            if (FAILED(svc->ExecNotificationQueryAsync(lang, query, WBEM_FLAG_SEND_STATUS, nullptr, sink))) {
                OutputDebugStringW(L"[hud] no brightness events here\n"); // a desktop monitor: windows has no panel to report on
                sink->Release();
                sink = nullptr;
            }
        }
        WaitForSingleObject(stop, INFINITE); // asleep until the option goes off: nothing polls
        if (sink) {
            sink->notify = nullptr;
            svc->CancelAsyncCall(sink);
            sink->Release();
        }
        svc.Reset();
        loc.Reset();
        SysFreeString(ns);
        SysFreeString(lang);
        SysFreeString(query);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach(); // it leaves by itself once told to; the state goes with its last owner
}

void BrightWatch::Stop() {
    if (impl) SetEvent(impl->stop);
    impl.reset();
}

// ---- the volume keys ----

namespace {
HWND keyTarget = nullptr; // the hook proc is static; only one hook ever exists
UINT keyMsg = 0;
std::atomic<bool> keysUsable{true}; // no speakers to change: the keys go on to windows instead of vanishing

LRESULT CALLBACK KeyProc(int code, WPARAM wp, LPARAM lp) { // keep this short: windows times out slow hooks
    if (code == HC_ACTION && keyTarget) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (keysUsable.load() && (k->vkCode == VK_VOLUME_UP || k->vkCode == VK_VOLUME_DOWN || k->vkCode == VK_VOLUME_MUTE)) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) PostMessageW(keyTarget, keyMsg, k->vkCode, 0); // applied on the ui thread, after we return
            return 1; // swallowed, so windows never shows its own popup
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}

bool VolumeKeys::Start(HWND notify, UINT msg) {
    if (hook) return true;
    keyTarget = notify;
    keyMsg = msg;
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(nullptr), 0);
    return hook != nullptr;
}

void VolumeKeys::Usable(bool on) { keysUsable.store(on); }

void VolumeKeys::Stop() {
    if (hook) UnhookWindowsHookEx(hook);
    hook = nullptr;
    keyTarget = nullptr;
}

}
