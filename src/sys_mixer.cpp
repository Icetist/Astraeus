#include "mixer.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <roapi.h>
#include <shellapi.h>
#include <winstring.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <unordered_map>

#pragma comment(lib, "version.lib") // GetFileVersionInfo, for an app's display name
#pragma comment(lib, "runtimeobject.lib") // RoGetActivationFactory, for per-app outputs

using Microsoft::WRL::ComPtr;

namespace sys {
namespace {

// windows' per-app output routing: an internal factory, the same the settings page uses. vtable order must match exactly, unused slots stay
MIDL_INTERFACE("ab3d4648-e242-459f-b02f-541c70306324")
IAudioPolicyConfigFactory : public IInspectable {
public:
    virtual HRESULT STDMETHODCALLTYPE Unused00() = 0; // add_CtxVolumeChange
    virtual HRESULT STDMETHODCALLTYPE Unused01() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused02() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused03() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused04() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused05() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused06() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused07() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused08() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused09() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused10() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused11() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused12() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused13() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused14() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused15() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused16() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused17() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused18() = 0; // remove_ChatContextChanged
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role, HSTRING device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role, HSTRING* device) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

constexpr wchar_t kRouteHead[] = L"\\\\?\\SWD#MMDEVAPI#", kRouteTail[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"; // how the factory spells an endpoint
constexpr int kMaxSessions = 128, kDevicesEvery = 5; // refreshes between re-listing the outputs: they rarely change

ComPtr<IAudioPolicyConfigFactory> Policy() { // made once; null on windows that lack it
    static ComPtr<IAudioPolicyConfigFactory> made;
    static bool tried = false;
    if (tried) return made;
    tried = true;
    static constexpr wchar_t kClass[] = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING_HEADER hdr;
    HSTRING name = nullptr;
    if (SUCCEEDED(WindowsCreateStringReference(kClass, ARRAYSIZE(kClass) - 1, &hdr, &name)))
        RoGetActivationFactory(name, __uuidof(IAudioPolicyConfigFactory), reinterpret_cast<void**>(made.GetAddressOf()));
    return made;
}

std::wstring RouteOf(DWORD pid) { // the endpoint id the app was sent to, empty when it follows the default
    auto pol = Policy();
    HSTRING s = nullptr;
    if (!pol || FAILED(pol->GetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, &s)) || !s) return {};
    std::wstring full = WindowsGetStringRawBuffer(s, nullptr);
    WindowsDeleteString(s);
    size_t a = full.find(L"MMDEVAPI#"), b = full.rfind(L"#{");
    if (a == std::wstring::npos || b == std::wstring::npos || b <= a + 9) return {};
    return full.substr(a + 9, b - a - 9);
}

std::wstring Describe(const std::wstring& path) { // the name an exe gives itself ("Spotify"), else its file name
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy); // 0 when the exe has no version resource
    if (size) {
        std::vector<BYTE> buf(size);
        struct Lang { WORD lang, page; }* langs = nullptr; // the layout of each \VarFileInfo\Translation entry
        UINT n = 0;
        if (GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) && VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &n) && n >= sizeof(Lang)) {
            wchar_t q[64];
            swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].page); // first language listed is the exe's own
            wchar_t* desc = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), q, (void**)&desc, &len) && desc && len > 1 && desc[0]) return std::wstring(desc, wcsnlen(desc, len)); // len counts the nul
        }
    }
    size_t slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.'); // fall back to the file name without its extension
    std::wstring stem = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    if (dot != std::wstring::npos && dot > slash) stem.resize(dot - (slash == std::wstring::npos ? 0 : slash + 1)); // a dot in a folder name isn't an extension
    if (!stem.empty()) stem[0] = (wchar_t)std::towupper(stem[0]);
    return stem;
}

std::shared_ptr<const std::vector<uint32_t>> IconOf(const std::wstring& path) { // the app's icon, 32 px, premultiplied bgra
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon) return nullptr; // large is 32 px at 100%
    constexpr int kPx = 32;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kPx, -kPx, 1, 32, BI_RGB}; // negative height: top-down rows
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    std::shared_ptr<std::vector<uint32_t>> out;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        DrawIconEx(dc, 0, 0, sfi.hIcon, kPx, kPx, 0, nullptr, DI_NORMAL); // draws with alpha into a zeroed dib, so it comes out premultiplied
        GdiFlush(); // gdi batches: make sure the bits are written before reading them
        out = std::make_shared<std::vector<uint32_t>>(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + kPx * kPx);
        bool alpha = false;
        for (uint32_t px : *out) alpha |= (px >> 24) != 0;
        if (!alpha) // an old icon with no alpha: whatever was drawn is opaque
            for (uint32_t& px : *out)
                if (px & 0xFFFFFF) px |= 0xFF000000;
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    if (dc) DeleteDC(dc);
    DestroyIcon(sfi.hIcon); // SHGFI_ICON hands over an icon the caller owns
    return out;
}

std::wstring PathOf(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[MAX_PATH];
    DWORD len = MAX_PATH;
    std::wstring path;
    if (QueryFullProcessImageNameW(proc, 0, buf, &len)) path.assign(buf, len);
    CloseHandle(proc);
    return path;
}

}

struct Mixer::Impl {
    ComPtr<IMMDeviceEnumerator> en;
    std::vector<ComPtr<IAudioSessionManager2>> mgrs; // one per active output: a routed app plays on its own
    int sinceDevices = 0;
    struct Session {
        std::wstring key;
        DWORD pid = 0;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioMeterInformation> meter;
        size_t app = 0; // its row in apps
    };
    std::vector<Session> sessions;
    std::vector<MixApp> apps;
    struct Known {
        std::wstring name;
        std::shared_ptr<const std::vector<uint32_t>> icon;
    };
    std::map<std::wstring, Known> known; // name and icon per exe, read once while open
    std::unordered_map<DWORD, std::wstring> paths; // pid -> exe path: a process is opened once, not every second
    std::unordered_map<DWORD, std::wstring> routes; // pid -> its output, asked once: only Route changes it
    std::wstring device;

    void ListDevices() {
        mgrs.clear();
        device.clear();
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &def))) {
            ComPtr<IPropertyStore> props; // the device's own name, for the header
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(def->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                device = pv.pwszVal;
            PropVariantClear(&pv);
        }
        ComPtr<IMMDeviceCollection> all;
        UINT n = 0;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) || FAILED(all->GetCount(&n))) return;
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> dev;
            ComPtr<IAudioSessionManager2> mgr;
            if (SUCCEEDED(all->Item(i, &dev)) && SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(mgr.GetAddressOf()))))
                mgrs.push_back(mgr);
        }
    }
};

Mixer::Mixer() = default;
Mixer::~Mixer() { Close(); }

bool Mixer::Open() {
    if (impl) return true;
    auto i = std::make_unique<Impl>();
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&i->en)))) return false;
    i->ListDevices();
    if (i->mgrs.empty()) return false;
    impl = std::move(i);
    Refresh();
    return true;
}

void Mixer::Close() { impl.reset(); }

std::wstring Mixer::Device() const { return impl ? impl->device : std::wstring(); }

bool Mixer::IsOpen() const { return impl != nullptr; }

const std::vector<MixApp>& Mixer::Apps() const {
    static const std::vector<MixApp> none;
    return impl ? impl->apps : none;
}

void Mixer::Refresh() {
    if (!impl) return;
    if (++impl->sinceDevices >= kDevicesEvery) impl->sinceDevices = 0, impl->ListDevices(); // a plugged-in headset joins within seconds
    std::vector<Impl::Session> sessions;
    std::vector<MixApp> apps;
    std::unordered_map<DWORD, std::wstring> paths, routes;
    const DWORD self = GetCurrentProcessId();
    for (auto& mgr : impl->mgrs) {
        ComPtr<IAudioSessionEnumerator> list;
        int count = 0;
        if (FAILED(mgr->GetSessionEnumerator(&list)) || FAILED(list->GetCount(&count))) continue;
        for (int k = 0; k < count && (int)sessions.size() < kMaxSessions; ++k) {
            ComPtr<IAudioSessionControl> ctl;
            ComPtr<IAudioSessionControl2> ctl2;
            ComPtr<ISimpleAudioVolume> vol;
            AudioSessionState state = AudioSessionStateExpired;
            if (FAILED(list->GetSession(k, &ctl)) || FAILED(ctl.As(&ctl2)) || FAILED(ctl.As(&vol)) || FAILED(ctl->GetState(&state)) || state == AudioSessionStateExpired) continue;
            std::wstring key, path;
            DWORD pid = 0;
            if (ctl2->IsSystemSoundsSession() == S_OK) {
                key = L"system";
            } else {
                if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;
                auto seen = impl->paths.find(pid);
                path = seen != impl->paths.end() ? seen->second : PathOf(pid);
                if (path.empty()) continue;
                paths[pid] = path;
                key = path;
                std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
            }
            auto known = impl->known.find(key);
            if (known == impl->known.end())
                known = impl->known.emplace(key, Impl::Known{key == L"system" ? L"System sounds" : Describe(path), key == L"system" ? nullptr : IconOf(path)}).first;
            float level = 1;
            BOOL muted = FALSE;
            vol->GetMasterVolume(&level);
            vol->GetMute(&muted);
            ComPtr<IAudioMeterInformation> meter;
            ctl.As(&meter); // absent on some sessions: the meter then reads silent
            auto app = std::find_if(apps.begin(), apps.end(), [&](const MixApp& a) { return a.key == key; }); // one row per app, however many sessions it opened
            if (app == apps.end()) {
                MixApp a{key, known->second.name, level, muted != FALSE, state == AudioSessionStateActive};
                a.icon = known->second.icon;
                if (pid) {
                    auto had = impl->routes.find(pid);
                    a.route = had != impl->routes.end() ? had->second : RouteOf(pid);
                    routes[pid] = a.route;
                }
                apps.push_back(std::move(a));
            } else {
                app->playing |= state == AudioSessionStateActive;
                app->muted &= muted != FALSE;
            }
            sessions.push_back({key, pid, vol, meter, 0});
        }
    }
    for (const auto& a : impl->apps) // peaks carry over until the next Meter
        for (auto& b : apps)
            if (a.key == b.key) b.peak = a.peak;
    std::stable_sort(apps.begin(), apps.end(), [](const MixApp& a, const MixApp& b) { // what's playing first, system sounds last
        if ((a.key == L"system") != (b.key == L"system")) return b.key == L"system";
        if (a.playing != b.playing) return a.playing;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    for (auto& s : sessions)
        for (size_t k = 0; k < apps.size(); ++k)
            if (apps[k].key == s.key) s.app = k;
    impl->sessions = std::move(sessions);
    impl->apps = std::move(apps);
    impl->paths = std::move(paths); // only the living: a reused pid is looked up afresh
    impl->routes = std::move(routes);
}

void Mixer::Meter() {
    if (!impl) return;
    for (auto& a : impl->apps) a.peak = 0;
    for (auto& s : impl->sessions) {
        float p = 0;
        if (s.meter && SUCCEEDED(s.meter->GetPeakValue(&p)) && s.app < impl->apps.size()) impl->apps[s.app].peak = (std::max)(impl->apps[s.app].peak, p);
    }
}

void Mixer::SetVolume(const std::wstring& key, float v) {
    if (!impl) return;
    v = (std::clamp)(v, 0.0f, 1.0f);
    for (auto& s : impl->sessions)
        if (s.key == key) s.volume->SetMasterVolume(v, nullptr);
    for (auto& a : impl->apps)
        if (a.key == key) a.volume = v; // shows at once, the next refresh confirms it
}

void Mixer::SetMuted(const std::wstring& key, bool muted) {
    if (!impl) return;
    for (auto& s : impl->sessions)
        if (s.key == key) s.volume->SetMute(muted, nullptr);
    for (auto& a : impl->apps)
        if (a.key == key) a.muted = muted;
}

bool Mixer::HasApp(const std::wstring& key) const {
    if (!impl) return false;
    for (const auto& a : impl->apps)
        if (a.key == key) return true;
    return false;
}

float Mixer::VolumeOf(const std::wstring& key) const {
    if (!impl) return -1;
    for (const auto& a : impl->apps)
        if (a.key == key) return a.volume;
    return -1;
}

bool Mixer::CanRoute() { return Policy() != nullptr; }

bool Mixer::Route(const std::wstring& key, const std::wstring& deviceId) {
    auto pol = Policy();
    if (!impl || !pol || key == L"system") return false;
    std::wstring full = deviceId.empty() ? std::wstring() : kRouteHead + deviceId + kRouteTail;
    HSTRING s = nullptr;
    if (!full.empty() && FAILED(WindowsCreateString(full.c_str(), (UINT32)full.size(), &s))) return false;
    bool ok = false;
    for (auto& ses : impl->sessions)
        if (ses.key == key && ses.pid)
            for (ERole role : {eConsole, eMultimedia}) // both, like the settings page: games and players ask for different roles
                ok |= SUCCEEDED(pol->SetPersistedDefaultAudioEndpoint(ses.pid, eRender, role, s)); // null clears it
    if (s) WindowsDeleteString(s);
    for (auto& a : impl->apps)
        if (a.key == key) a.route = deviceId;
    for (auto& ses : impl->sessions)
        if (ses.key == key && ses.pid) impl->routes[ses.pid] = deviceId;
    return ok;
}

}
