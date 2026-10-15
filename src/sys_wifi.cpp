#include "wifi.h"

#include <wlanapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#pragma comment(lib, "wlanapi.lib")

namespace sys {
namespace {

constexpr ULONGLONG kOpMs = 15000; // stop showing "connecting" after this
constexpr ULONGLONG kScanMs = 10000; // scans normally finish in ~4s, never spin forever

void Log(const wchar_t* what) { OutputDebugStringW((std::wstring(L"[wifi] ") + what + L"\n").c_str()); }

std::wstring Utf8(const std::string& s) { // ssids are bytes, nearly always utf-8
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end()); // not utf-8, show bytes as latin-1
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

}

struct Wifi::Impl {
    HWND hwnd = nullptr;
    UINT msg = 0;
    HANDLE h = nullptr; // wlan client handle
    GUID iface{};
    std::atomic<bool> scanning{false};
    std::atomic<ULONGLONG> scanUntil{0};
    std::atomic<WifiState> state{WifiState::Ok};

    mutable std::mutex mu; // guards nets + pending
    std::vector<WifiNet> nets;
    std::string pendingRaw; // ssid of the in-flight op
    ULONGLONG pendingUntil = 0;
    std::atomic<bool> ready{false}; // h + iface usable, set by the starter thread
    std::thread starter; // WlanOpenHandle takes ~200ms, keep it off the ui thread

    ~Impl() {
        if (starter.joinable()) starter.join(); // closed while still opening
        if (!h) return;
        WlanRegisterNotification(h, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr); // stop events first
        WlanCloseHandle(h, nullptr); // waits for in-flight callbacks
    }

    void Ping() const {
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, msg, 0, 0); // ui re-snaps on this
    }

    bool Start() {
        DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) { // 2 = vista+ client api
            h = nullptr;
            state = WifiState::NoAdapter;
            return false;
        }
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS || !list || list->dwNumberOfItems == 0) {
            if (list) WlanFreeMemory(list);
            state = WifiState::NoAdapter;
            return false;
        }
        DWORD pick = 0;
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i)
            if (list->InterfaceInfo[i].isState == wlan_interface_state_connected) pick = i; // prefer the one in use
        iface = list->InterfaceInfo[pick].InterfaceGuid;
        WlanFreeMemory(list);
        if (WlanRegisterNotification(h, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, OnEvent, this, nullptr, nullptr) != ERROR_SUCCESS)
            Log(L"event registration failed, list only refreshes on open");
        Refresh();
        return true;
    }

    static void WINAPI OnEvent(PWLAN_NOTIFICATION_DATA d, PVOID ctx) { // wlan service thread
        auto* s = static_cast<Impl*>(ctx);
        if (!s || !d || d->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM) return;
        switch (d->NotificationCode) {
        case wlan_notification_acm_scan_complete:
        case wlan_notification_acm_scan_fail:
            s->scanning = false;
            break;
        case wlan_notification_acm_connection_complete:
        case wlan_notification_acm_connection_attempt_fail:
        case wlan_notification_acm_disconnected: { // only the event for the network we wait on ends the spinner
            std::string raw;
            if (d->pData && d->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA)) {
                const auto* c = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(d->pData);
                raw.assign(reinterpret_cast<const char*>(c->dot11Ssid.ucSSID), c->dot11Ssid.uSSIDLength);
            }
            std::lock_guard lock(s->mu);
            if (raw.empty() || raw == s->pendingRaw) s->pendingRaw.clear(); // switching networks also disconnects the old one
            break;
        }
        case wlan_notification_acm_scan_list_refresh:
        case wlan_notification_acm_network_available:
        case wlan_notification_acm_network_not_available:
        case wlan_notification_acm_connection_start:
            break;
        default:
            return; // nothing visible changed
        }
        try {
            s->Refresh();
        } catch (...) {
            Log(L"refresh failed");
        }
        s->Ping();
    }

    void Refresh() {
        PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
        DWORD rc = WlanGetAvailableNetworkList(h, &iface, 0, nullptr, &list);
        if (rc == ERROR_ACCESS_DENIED) { // 24h2+: location access off
            state = WifiState::Denied;
            std::lock_guard lock(mu);
            nets.clear();
            return;
        }
        if (rc != ERROR_SUCCESS || !list) return;
        state = WifiState::Ok;
        std::map<std::string, WifiNet> merged; // a network shows once per profile plus once raw, fold them
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            const WLAN_AVAILABLE_NETWORK& a = list->Network[i];
            if (a.dot11Ssid.uSSIDLength == 0) continue; // hidden network, nothing to show
            std::string raw(reinterpret_cast<const char*>(a.dot11Ssid.ucSSID), a.dot11Ssid.uSSIDLength);
            WifiNet& n = merged[raw];
            if (n.raw.empty()) {
                n.raw = raw;
                n.ssid = Utf8(raw);
                n.infra = a.dot11BssType != dot11_BSS_type_independent;
            }
            n.signal = (std::max)(n.signal, (int)a.wlanSignalQuality);
            n.secured = n.secured || a.bSecurityEnabled;
            if (a.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) n.on = true;
            if ((a.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) && n.profile.empty()) n.profile = a.strProfileName;
        }
        WlanFreeMemory(list);
        std::vector<WifiNet> out;
        out.reserve(merged.size());
        for (auto& [raw, n] : merged) out.push_back(std::move(n));
        std::lock_guard lock(mu);
        nets = std::move(out);
    }
};

Wifi::Wifi() = default;
Wifi::~Wifi() { Close(); }

void Wifi::Open(HWND hwnd, UINT msg) {
    if (impl) return;
    try {
        auto s = std::make_unique<Impl>();
        s->hwnd = hwnd;
        s->msg = msg;
        s->scanning = true; // sheet shows "looking for networks" while the handle opens
        s->scanUntil = GetTickCount64() + kScanMs;
        Impl* p = s.get();
        s->starter = std::thread([p] {
            try {
                if (p->Start()) {
                    p->ready = true;
                    if (WlanScan(p->h, &p->iface, nullptr, nullptr, nullptr) != ERROR_SUCCESS) p->scanning = false;
                } else {
                    p->scanning = false;
                }
            } catch (...) {
                p->scanning = false;
                Log(L"start failed");
            }
            p->Ping();
        });
        impl = std::move(s); // kept even on failure so State() can explain
    } catch (...) {
        Log(L"open failed");
    }
}

void Wifi::Close() { // teardown can wait on the wlan service, keep it off the ui thread
    if (!impl) return;
    try {
        std::thread([p = impl.release()] { delete p; }).detach(); // impl only posts to hwnd if it still exists
    } catch (...) {
        impl.reset(); // no thread available: tear down inline
    }
}

void Wifi::Scan() {
    if (!impl || !impl->ready) return;
    if (WlanScan(impl->h, &impl->iface, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        impl->scanning = true;
        impl->scanUntil = GetTickCount64() + kScanMs;
    }
}

bool Wifi::Scanning() const { return impl && impl->scanning && GetTickCount64() < impl->scanUntil; }

WifiState Wifi::State() const { return impl ? impl->state.load() : WifiState::Ok; }

void Wifi::Snap(std::vector<WifiNet>& out) const {
    out.clear();
    if (!impl) return;
    {
        std::lock_guard lock(impl->mu);
        out = impl->nets;
        const bool live = !impl->pendingRaw.empty() && GetTickCount64() < impl->pendingUntil;
        for (auto& n : out) n.pending = live && n.raw == impl->pendingRaw;
    }
    std::sort(out.begin(), out.end(), [](const WifiNet& a, const WifiNet& b) { // connected, saved, strongest
        if (a.on != b.on) return a.on;
        if (a.profile.empty() != b.profile.empty()) return !a.profile.empty();
        if (a.signal != b.signal) return a.signal > b.signal;
        return _wcsicmp(a.ssid.c_str(), b.ssid.c_str()) < 0;
    });
}

bool Wifi::Connect(const WifiNet& n) {
    if (!impl || !impl->ready) return false;
    if (n.profile.empty() && n.secured) return false; // needs a password, windows' own flyout asks for it
    DOT11_SSID ssid{};
    ssid.uSSIDLength = (ULONG)(std::min)(n.raw.size(), sizeof(ssid.ucSSID));
    std::memcpy(ssid.ucSSID, n.raw.data(), ssid.uSSIDLength);
    WLAN_CONNECTION_PARAMETERS cp{};
    cp.dot11BssType = n.infra ? dot11_BSS_type_infrastructure : dot11_BSS_type_independent;
    if (!n.profile.empty()) {
        cp.wlanConnectionMode = wlan_connection_mode_profile;
        cp.strProfile = n.profile.c_str();
    } else {
        cp.wlanConnectionMode = wlan_connection_mode_discovery_unsecure; // open network, windows makes a temp profile
        cp.pDot11Ssid = &ssid;
    }
    {
        std::lock_guard lock(impl->mu); // before the call: the completion event can beat WlanConnect's return
        impl->pendingRaw = n.raw;
        impl->pendingUntil = GetTickCount64() + kOpMs;
    }
    if (WlanConnect(impl->h, &impl->iface, &cp, nullptr) == ERROR_SUCCESS) return true; // async, events report the result
    std::lock_guard lock(impl->mu);
    impl->pendingRaw.clear();
    return false;
}

void Wifi::Disconnect() {
    if (!impl || !impl->ready) return;
    std::string raw;
    {
        std::lock_guard lock(impl->mu);
        for (const auto& n : impl->nets)
            if (n.on) raw = n.raw;
    }
    {
        std::lock_guard lock(impl->mu); // before the call, same reason as Connect
        impl->pendingRaw = raw;
        impl->pendingUntil = GetTickCount64() + kOpMs;
    }
    if (WlanDisconnect(impl->h, &impl->iface, nullptr) == ERROR_SUCCESS) return;
    std::lock_guard lock(impl->mu);
    impl->pendingRaw.clear();
}

}
