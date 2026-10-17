#include "mixer.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <map>

#pragma comment(lib, "version.lib")

using Microsoft::WRL::ComPtr;

namespace sys {
namespace {

std::wstring Describe(const std::wstring& path) { // the name an exe gives itself ("Spotify"), else its file name
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size) {
        std::vector<BYTE> buf(size);
        struct Lang { WORD lang, page; }* langs = nullptr;
        UINT n = 0;
        if (GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) && VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &n) && n >= sizeof(Lang)) {
            wchar_t q[64];
            swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].page);
            wchar_t* desc = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), q, (void**)&desc, &len) && desc && len > 1 && desc[0]) return std::wstring(desc, wcsnlen(desc, len));
        }
    }
    size_t slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
    std::wstring stem = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    if (dot != std::wstring::npos && dot > slash) stem.resize(dot - (slash == std::wstring::npos ? 0 : slash + 1));
    if (!stem.empty()) stem[0] = (wchar_t)std::towupper(stem[0]);
    return stem;
}

std::shared_ptr<const std::vector<uint32_t>> IconOf(const std::wstring& path) { // the app's icon, 32 px, premultiplied bgra
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon) return nullptr;
    constexpr int kPx = 32;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kPx, -kPx, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    std::shared_ptr<std::vector<uint32_t>> out;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        DrawIconEx(dc, 0, 0, sfi.hIcon, kPx, kPx, 0, nullptr, DI_NORMAL);
        GdiFlush();
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
    DestroyIcon(sfi.hIcon);
    return out;
}

}

struct Mixer::Impl {
    ComPtr<IAudioSessionManager2> mgr;
    struct Session {
        std::wstring key;
        ComPtr<ISimpleAudioVolume> volume;
    };
    std::vector<Session> sessions;
    std::vector<MixApp> apps;
    struct Known {
        std::wstring name;
        std::shared_ptr<const std::vector<uint32_t>> icon;
    };
    std::map<std::wstring, Known> known; // name and icon per exe, read once while open
};

Mixer::Mixer() = default;
Mixer::~Mixer() { Close(); }

bool Mixer::Open() {
    if (impl) return true;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    auto i = std::make_unique<Impl>();
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(i->mgr.GetAddressOf()))))
        return false;
    impl = std::move(i);
    Refresh();
    return true;
}

void Mixer::Close() { impl.reset(); }

bool Mixer::IsOpen() const { return impl != nullptr; }

const std::vector<MixApp>& Mixer::Apps() const {
    static const std::vector<MixApp> none;
    return impl ? impl->apps : none;
}

void Mixer::Refresh() {
    if (!impl) return;
    ComPtr<IAudioSessionEnumerator> list;
    int count = 0;
    if (FAILED(impl->mgr->GetSessionEnumerator(&list)) || FAILED(list->GetCount(&count))) return;
    std::vector<Impl::Session> sessions;
    std::vector<MixApp> apps;
    const DWORD self = GetCurrentProcessId();
    for (int k = 0; k < count && k < 128; ++k) {
        ComPtr<IAudioSessionControl> ctl;
        ComPtr<IAudioSessionControl2> ctl2;
        ComPtr<ISimpleAudioVolume> vol;
        AudioSessionState state = AudioSessionStateExpired;
        if (FAILED(list->GetSession(k, &ctl)) || FAILED(ctl.As(&ctl2)) || FAILED(ctl.As(&vol)) || FAILED(ctl->GetState(&state)) || state == AudioSessionStateExpired) continue;
        std::wstring key, path;
        if (ctl2->IsSystemSoundsSession() == S_OK) {
            key = L"system";
        } else {
            DWORD pid = 0;
            if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!proc) continue;
            wchar_t buf[MAX_PATH];
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, buf, &len)) path.assign(buf, len);
            CloseHandle(proc);
            if (path.empty()) continue;
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
        sessions.push_back({key, vol});
        auto app = std::find_if(apps.begin(), apps.end(), [&](const MixApp& a) { return a.key == key; }); // one row per app, however many sessions it opened
        if (app == apps.end()) {
            apps.push_back({key, known->second.name, level, muted != FALSE, state == AudioSessionStateActive, known->second.icon});
        } else {
            app->playing |= state == AudioSessionStateActive;
            app->muted &= muted != FALSE;
        }
    }
    std::stable_sort(apps.begin(), apps.end(), [](const MixApp& a, const MixApp& b) { // what's playing first, system sounds last
        if ((a.key == L"system") != (b.key == L"system")) return b.key == L"system";
        if (a.playing != b.playing) return a.playing;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    impl->sessions = std::move(sessions);
    impl->apps = std::move(apps);
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

}
