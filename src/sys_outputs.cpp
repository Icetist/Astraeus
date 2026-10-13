#include <windows.h>
#include <mmreg.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <powrprof.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

#include "sys.h"

using Microsoft::WRL::ComPtr;

namespace {

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8") // undocumented, iid for win7+
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0; // vtable order must match exactly, unused slots stay
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) = 0; // the only method we call
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

const CLSID kPolicyCls = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}}; // CPolicyConfigClient
const PROPERTYKEY kNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14}; // PKEY_Device_FriendlyName, avoids functiondiscoverykeys header

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring ShortName(const std::wstring& full) { // "Speakers (Realtek Audio)" -> "Speakers"
    size_t paren = full.find(L" (");
    std::wstring s = paren == std::wstring::npos || paren == 0 ? full : full.substr(0, paren); // keep names that start with "("
    return s.empty() ? full : s;
}

bool IsPhones(const std::wstring& full) { // name heuristic, form factor prop is unreliable
    std::wstring l = Lower(full);
    for (const wchar_t* k : {L"headphone", L"headset", L"buds", L"airpods", L"earphone", L"hands-free"})
        if (l.find(k) != std::wstring::npos) return true;
    return false;
}

}

namespace sys {

std::vector<OutDev> ListOuts(std::wstring& defaultId) {
    std::vector<OutDev> out;
    defaultId.clear();
    ComPtr<IMMDeviceEnumerator> devEnum;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devEnum))) ||
        FAILED(devEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) // active only, skip unplugged/disabled
        return out;

    ComPtr<IMMDevice> def;
    LPWSTR defId = nullptr;
    if (SUCCEEDED(devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && SUCCEEDED(def->GetId(&defId))) {
        defaultId = defId;
        CoTaskMemFree(defId); // GetId allocates with CoTaskMemAlloc
    }

    UINT count = 0;
    devices->GetCount(&count); // stays 0 on failure
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> props;
        LPWSTR id = nullptr;
        if (FAILED(devices->Item(i, &device)) || FAILED(device->GetId(&id))) continue; // skip bad entries
        OutDev d;
        d.id = id; // copy before freeing
        CoTaskMemFree(id);
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
            SUCCEEDED(props->GetValue(kNameKey, &name)) && name.vt == VT_LPWSTR) { // check type before pwszVal
            d.name = ShortName(name.pwszVal);
            d.headphones = IsPhones(name.pwszVal); // test full name, suffix has the hints
        }
        PropVariantClear(&name); // frees the string
        if (d.name.empty()) d.name = L"Output " + std::to_wstring(i + 1); // fallback label, 1-based
        out.push_back(std::move(d));
    }
    return out;
}

bool SetOut(const std::wstring& id) {
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kPolicyCls, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy)))) return false;
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) // set all roles like the sound panel does
        ok = SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), role)) && ok; // call first so every role is tried
    return ok;
}

struct Meter::Impl {
    ComPtr<IAudioMeterInformation> meter;
    UINT channels = 0;
};

Meter::Meter() : impl(std::make_unique<Impl>()) {}
Meter::~Meter() = default;

bool Meter::Refresh() { // rebind to default output, call on device change
    impl->meter.Reset();
    impl->channels = 0;
    ComPtr<IMMDeviceEnumerator> devEnum;
    ComPtr<IMMDevice> device;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devEnum))) ||
        FAILED(devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)impl->meter.GetAddressOf()))) {
        impl->meter.Reset();
        return false;
    }
    impl->meter->GetMeteringChannelCount(&impl->channels); // decides stereo vs mono read
    return true;
}

bool Meter::Read(float& left, float& right) { // peaks are 0..1 since last call
    left = right = 0;
    if (!impl->meter) return false;
    float peaks[8] = {}; // up to 7.1
    if (impl->channels >= 2 && impl->channels <= 8 && SUCCEEDED(impl->meter->GetChannelsPeakValues(impl->channels, peaks))) {
        left = peaks[0]; // channels 0/1 are front left/right
        right = peaks[1];
        return true;
    }
    float peak = 0; // mono or >8 channels: one overall peak
    if (FAILED(impl->meter->GetPeakValue(&peak))) return false;
    left = right = peak;
    return true;
}

bool IsPortable() {
    SYSTEM_POWER_CAPABILITIES caps = {};
    if (!GetPwrCapabilities(&caps)) return false;
    return caps.SystemBatteriesPresent && !caps.BatteriesAreShortTerm; // short-term = ups, not a laptop
}

}
