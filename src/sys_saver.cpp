// Energy saver, ported from halobar's toggles: settings' own switch, published through its wnf state.
#include "saver.h"

#include <windows.h>
#include <objbase.h>
#include <powrprof.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace sys {
namespace {

// no public setter exists. settings' own "always use energy saver" switch publishes a dword to this wnf
// state (1 = always on, 2 = automatic by battery threshold); the power service persists it. publishing needs no admin.
constexpr ULONGLONG kWnfEnergySaver = 0x41c6013da3bc3075ull; // from SettingsHandlers_OneCore_BatterySaver.dll
constexpr DWORD kSaverAlways = 1, kSaverAuto = 2; // the two values settings itself writes
constexpr GUID kSaverStatus = {0x550e8400, 0xe29b, 0x41d4, {0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00}}; // GUID_ENERGY_SAVER_STATUS, without initguid

using WnfQuery = LONG(NTAPI*)(const ULONGLONG*, const void*, const void*, ULONG*, void*, ULONG*); // undocumented: state name, type id, scope, stamp, buffer, size
using WnfPublish = LONG(NTAPI*)(ULONGLONG, const void*, const void*, ULONG, const void*); // name goes by value

struct Wnf {
    WnfQuery query = nullptr;
    WnfPublish publish = nullptr;
    Wnf() {
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
            query = reinterpret_cast<WnfQuery>(GetProcAddress(nt, "NtQueryWnfStateData")); // looked up at runtime, so a build without them still starts
            publish = reinterpret_cast<WnfPublish>(GetProcAddress(nt, "RtlPublishWnfStateData"));
        }
    }
};

const Wnf& Ntdll() {
    static const Wnf w;
    return w;
}

bool SaverMode(DWORD& mode) { // false on builds without the state (pre 24h2) or a shape we don't know
    const Wnf& w = Ntdll();
    if (!w.query) return false;
    DWORD v = 0;
    ULONG stamp = 0, size = sizeof(v);
    if (w.query(&kWnfEnergySaver, nullptr, nullptr, &stamp, &v, &size) < 0 || size != sizeof(v)) return false; // negative ntstatus: no such state here
    if (v != kSaverAlways && v != kSaverAuto) return false; // an unknown value: don't pretend to understand it
    mode = v;
    return true;
}

std::atomic<int> gSaverStatus{-1}; // ENERGY_SAVER_STATUS from the power notification, -1 until it lands

ULONG CALLBACK OnSaverStatus(PVOID, ULONG type, PVOID setting) { // runs on a system thread, hence the atomic
    if (type != PBT_POWERSETTINGCHANGE || !setting) return ERROR_SUCCESS;
    const auto* s = static_cast<const POWERBROADCAST_SETTING*>(setting);
    if (s->PowerSetting == kSaverStatus && s->DataLength >= sizeof(DWORD))
        gSaverStatus.store((int)*reinterpret_cast<const DWORD*>(s->Data));
    return ERROR_SUCCESS;
}

void WatchSaver() { // once per process, on first use; the registration posts the current value right away
    static std::once_flag once;
    std::call_once(once, [] {
        static DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS params{OnSaverStatus, nullptr}; // static: must outlive the registration
        HPOWERNOTIFY h = nullptr; // kept for the process lifetime on purpose
        if (PowerSettingRegisterNotification(&kSaverStatus, DEVICE_NOTIFY_CALLBACK, &params, &h) != ERROR_SUCCESS)
            OutputDebugStringW(L"[saver] energy saver status unavailable\n"); // pre 24h2
    });
}

void ShellAsync(const wchar_t* target) { // shellexecute can stall on a cold settings app, keep it off the ui thread
    std::wstring t = target;
    std::thread([t] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = t.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) OutputDebugStringW(L"[saver] ShellExecuteExW failed\n");
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
}

}

bool SaverAvailable() {
    DWORD mode = 0;
    if (SaverMode(mode)) return true; // 24h2+, works on desktops too
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & BATTERY_FLAG_NO_BATTERY) && ps.BatteryFlag != BATTERY_FLAG_UNKNOWN; // older builds: battery saver needs a battery
}

bool SaverOn() {
    WatchSaver();
    const int st = gSaverStatus.load();
    if (st >= 0) return st != ENERGY_SAVER_OFF; // the flag below stays 0 on ac even when energy saver is on
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && ps.SystemStatusFlag == 1; // until the notification lands: right on battery at least
}

bool SetSaver(bool on) {
    DWORD mode = 0;
    const Wnf& w = Ntdll();
    if (SaverMode(mode) && w.publish) {
        const DWORD want = on ? kSaverAlways : kSaverAuto; // auto still engages below the user's battery threshold
        if (mode == want && on) return true;
        if (mode == want) { // already automatic, yet on: the battery is under the threshold, and only settings can move that
            ShellAsync(L"ms-settings:batterysaver");
            return false;
        }
        if (w.publish(kWnfEnergySaver, nullptr, &want, sizeof(want), nullptr) >= 0) return true;
        OutputDebugStringW(L"[saver] energy saver publish refused\n");
    }
    ShellAsync(L"ms-settings:batterysaver"); // older builds: battery saver has no user-mode switch
    return false;
}

}
