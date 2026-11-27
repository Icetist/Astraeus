#include "sys_toggles.h"

#include <windows.h>
#include <shellapi.h>

#include <ctime>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

namespace sys::toggles {

namespace {

// night light's state, a small record windows keeps in its cloud store
constexpr wchar_t kNightKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
                                L"default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data.bluelightreduction.bluelightreductionstate";
constexpr size_t kNightFlag = 18, kNightGap = 23, kNightStamp = 10, kNightStampLen = 5; // the switch byte; where "on" adds two bytes; when it last changed
constexpr BYTE kNightOn = 0x15, kNightOff = 0x13;
constexpr size_t kNightOffLen = 41;
constexpr BYTE kNightPad[2] = {0x10, 0x00};
constexpr wchar_t kCdpKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CDP";

std::vector<BYTE> NightData() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kNightKey, L"Data", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS || !size) return {};
    std::vector<BYTE> d(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kNightKey, L"Data", RRF_RT_REG_BINARY, nullptr, d.data(), &size) != ERROR_SUCCESS) return {};
    d.resize(size);
    return d;
}

template <typename F>
bool EachHdrPath(F f) { // every active screen that can show hdr
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return false;
    bool any = false;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
        info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        info.header.size = sizeof(info);
        info.header.adapterId = paths[i].targetInfo.adapterId;
        info.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS || !info.advancedColorSupported || info.advancedColorForceDisabled) continue;
        any = true;
        f(paths[i], info.advancedColorEnabled != 0);
    }
    return any;
}

winrt::Windows::Networking::NetworkOperators::NetworkOperatorTetheringManager Tether() {
    using namespace winrt::Windows::Networking;
    auto profile = Connectivity::NetworkInformation::GetInternetConnectionProfile();
    if (!profile) return nullptr;
    if (NetworkOperators::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile) != NetworkOperators::TetheringCapability::Enabled) return nullptr;
    return NetworkOperators::NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
}

}

bool NightLight() {
    std::vector<BYTE> d = NightData();
    return d.size() > kNightFlag && d[kNightFlag] == kNightOn;
}

bool SetNightLight(bool on) {
    std::vector<BYTE> d = NightData();
    if (d.size() < kNightOffLen || (d[kNightFlag] != kNightOn && d[kNightFlag] != kNightOff)) return false; // a shape we don't know: left alone
    bool was = d[kNightFlag] == kNightOn;
    if (was == on) return true;
    if (on) d.insert(d.begin() + kNightGap, std::begin(kNightPad), std::end(kNightPad));
    else d.erase(d.begin() + kNightGap, d.begin() + kNightGap + sizeof(kNightPad));
    d[kNightFlag] = on ? kNightOn : kNightOff;
    unsigned long long now = (unsigned long long)std::time(nullptr); // its "changed" time, as the 7-bit groups windows stores, so it takes the new state
    for (size_t i = 0; i < kNightStampLen; ++i, now >>= 7) d[kNightStamp + i] = (BYTE)((now & 0x7f) | (i + 1 < kNightStampLen ? 0x80 : 0));
    return RegSetKeyValueW(HKEY_CURRENT_USER, kNightKey, L"Data", REG_BINARY, d.data(), (DWORD)d.size()) == ERROR_SUCCESS;
}

bool HdrCan() {
    return EachHdrPath([](const DISPLAYCONFIG_PATH_INFO&, bool) {});
}

bool Hdr() {
    bool all = true;
    bool any = EachHdrPath([&](const DISPLAYCONFIG_PATH_INFO&, bool on) { all = all && on; });
    return any && all;
}

bool SetHdr(bool on) {
    bool ok = true;
    EachHdrPath([&](const DISPLAYCONFIG_PATH_INFO& p, bool now) {
        if (now == on) return;
        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE set{};
        set.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
        set.header.size = sizeof(set);
        set.header.adapterId = p.targetInfo.adapterId;
        set.header.id = p.targetInfo.id;
        set.enableAdvancedColor = on ? 1 : 0;
        ok = DisplayConfigSetDeviceInfo(&set.header) == ERROR_SUCCESS && ok;
    });
    return ok;
}

Spot Hotspot() {
    try {
        auto t = Tether();
        if (!t) return Spot::None;
        using St = winrt::Windows::Networking::NetworkOperators::TetheringOperationalState;
        St s = t.TetheringOperationalState();
        return s == St::On ? Spot::On : s == St::InTransition ? Spot::Busy : s == St::Off ? Spot::Off : Spot::None;
    } catch (...) {
        return Spot::None;
    }
}

bool SetHotspot(bool on) {
    try {
        auto t = Tether();
        if (!t) return false;
        auto r = on ? t.StartTetheringAsync().get() : t.StopTetheringAsync().get();
        return r.Status() == winrt::Windows::Networking::NetworkOperators::TetheringOperationStatus::Success;
    } catch (...) {
        return false;
    }
}

bool Nearby() {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kCdpKey, L"NearShareChannelUserAuthzPolicy", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v != 0;
}

void OpenNearby() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:crossdevice", nullptr, nullptr, SW_SHOWNORMAL);
}

}
