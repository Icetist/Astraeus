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

int Band(ULONG kHz) { return kHz == 0 ? 0 : kHz < 3000000 ? 2 : kHz < 5925000 ? 5 : 6; }

int Gen(int phy) { // dot11 phy type to the wi-fi generation people know: ht 7, vht 8, he 10, eht 11
    return phy == 7 ? 4 : phy == 8 ? 5 : phy == 10 ? 6 : phy == 11 ? 7 : 0;
}

const wchar_t* Auth(int algo, bool secured) { // dot11 auth algorithm, by number so older sdks build too
    switch (algo) {
    case 4: case 3: return L"WPA";
    case 7: return L"WPA2";
    case 6: return L"WPA2-Enterprise";
    case 8: case 11: return L"WPA3-Enterprise";
    case 9: return L"WPA3";
    case 10: return L"Enhanced Open";
    default: return secured ? L"Secured" : L"Open";
    }
}

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
    std::string keyRaw, badKeyRaw; // a join with a typed key in flight, and the last one that failed
    std::wstring keyProfile; // the profile that join made, removed again if the key was wrong
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
            std::wstring drop; // a profile to remove, done outside the lock
            {
                std::lock_guard lock(s->mu);
                if (raw.empty() || raw == s->pendingRaw) s->pendingRaw.clear(); // switching networks also disconnects the old one
                if (!s->keyRaw.empty() && raw == s->keyRaw && d->NotificationCode != wlan_notification_acm_disconnected) {
                    const auto* c = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(d->pData);
                    DWORD why = c ? c->wlanReasonCode : 0;
                    bool ok = d->NotificationCode == wlan_notification_acm_connection_complete && why == WLAN_REASON_CODE_SUCCESS;
                    if (!ok) { // didn't join: leave no profile behind; only a security failure is blamed on the password
                        drop = s->keyProfile;
                        if (why >= WLAN_REASON_CODE_MSMSEC_BASE && why <= WLAN_REASON_CODE_MSMSEC_END) s->badKeyRaw = s->keyRaw;
                    }
                    s->keyRaw.clear();
                    s->keyProfile.clear();
                }
            }
            if (!drop.empty()) WlanDeleteProfile(s->h, &s->iface, drop.c_str(), nullptr);
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

    void Details(std::map<std::string, WifiNet>& merged) const { // each network's band, and how the connected one is linked
        PWLAN_BSS_LIST bss = nullptr;
        std::map<std::string, LONG> best; // strongest access point seen per network
        DOT11_MAC_ADDRESS on{};
        bool haveOn = false;
        PWLAN_CONNECTION_ATTRIBUTES ca = nullptr;
        DWORD size = 0;
        if (WlanQueryInterface(h, &iface, wlan_intf_opcode_current_connection, nullptr, &size, reinterpret_cast<PVOID*>(&ca), nullptr) == ERROR_SUCCESS && ca) {
            if (ca->isState == wlan_interface_state_connected) {
                const auto& as = ca->wlanAssociationAttributes;
                std::string raw(reinterpret_cast<const char*>(as.dot11Ssid.ucSSID), as.dot11Ssid.uSSIDLength);
                auto it = merged.find(raw);
                if (it != merged.end()) {
                    it->second.gen = Gen((int)as.dot11PhyType);
                    it->second.rate = (int)((std::max)(as.ulRxRate, as.ulTxRate) / 1000); // kbps to Mbps
                    it->second.auth = Auth((int)ca->wlanSecurityAttributes.dot11AuthAlgorithm, ca->wlanSecurityAttributes.bSecurityEnabled != FALSE);
                    std::memcpy(on, as.dot11Bssid, sizeof(on));
                    haveOn = true;
                }
            }
            WlanFreeMemory(ca);
        }
        if (WlanGetNetworkBssList(h, &iface, nullptr, dot11_BSS_type_any, FALSE, nullptr, &bss) != ERROR_SUCCESS || !bss) return;
        for (DWORD i = 0; i < bss->dwNumberOfItems; ++i) {
            const WLAN_BSS_ENTRY& e = bss->wlanBssEntries[i];
            std::string raw(reinterpret_cast<const char*>(e.dot11Ssid.ucSSID), e.dot11Ssid.uSSIDLength);
            auto it = merged.find(raw);
            if (it == merged.end()) continue;
            bool mine = haveOn && it->second.on && !std::memcmp(e.dot11Bssid, on, sizeof(on)); // the access point we're actually on
            auto b = best.find(raw);
            if (mine || b == best.end() || e.lRssi > b->second) {
                it->second.band = Band(e.ulChCenterFrequency);
                best[raw] = mine ? 1000 : e.lRssi; // nothing outranks the one we're on
            }
        }
        WlanFreeMemory(bss);
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
            if (!n.authAlgo) n.authAlgo = (int)a.dot11DefaultAuthAlgorithm, n.cipher = (int)a.dot11DefaultCipherAlgorithm;
        }
        WlanFreeMemory(list);
        Details(merged);
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

namespace {
const wchar_t* AuthName(int algo) { // dot11 auth algorithm to the profile schema's name, personal networks only
    return algo == 4 ? L"WPAPSK" : algo == 7 ? L"WPA2PSK" : algo == 9 ? L"WPA3SAE" : nullptr;
}

std::wstring Xml(const std::wstring& s) { // escapes for an xml text node
    std::wstring o;
    for (wchar_t ch : s) {
        switch (ch) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        case L'\'': o += L"&apos;"; break;
        default: o += ch;
        }
    }
    return o;
}

void Wipe(std::wstring& s) { // passwords don't linger in freed memory
    if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    s.clear();
}
}

bool Wifi::CanJoin(const WifiNet& n) const { return n.infra && n.secured && n.profile.empty() && AuthName(n.authAlgo); }

bool Wifi::Join(const WifiNet& n, std::wstring& key) {
    const wchar_t* auth = AuthName(n.authAlgo);
    if (!impl || !impl->ready || !auth || key.size() < 8 || key.size() > 63) { // wpa passphrases are 8 to 63 characters
        Wipe(key);
        return false;
    }
    std::wstring name; // a profile name can't hold \\ / : * ? " < > |, the ssid itself goes in as hex
    for (wchar_t ch : n.ssid) name += std::wcschr(L"\\/:*?\"<>|", ch) || ch < 32 ? L' ' : ch;
    name = name.substr(0, 200);
    {
        LPWSTR existing = nullptr;
        DWORD flags = 0;
        if (WlanGetProfile(impl->h, &impl->iface, name.c_str(), nullptr, &existing, &flags, nullptr) == ERROR_SUCCESS) { // the name is taken: never replace someone's profile
            WlanFreeMemory(existing);
            Wipe(key);
            return false;
        }
    }
    std::wstring hex;
    for (unsigned char b : n.raw) {
        wchar_t buf[3];
        swprintf_s(buf, L"%02X", b);
        hex += buf;
    }
    const wchar_t* enc = n.cipher == 2 ? L"TKIP" : L"AES"; // ccmp (4) and anything newer is aes
    std::wstring xml, safeKey = Xml(key);
    xml.reserve(1024 + safeKey.size()); // one buffer, so no stray copies of the key are left in freed memory
    xml += L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>";
    xml += Xml(name);
    xml += L"</name><SSIDConfig><SSID><hex>";
    xml += hex;
    xml += L"</hex></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>auto</connectionMode><MSM><security><authEncryption><authentication>";
    xml += auth;
    xml += L"</authentication><encryption>";
    xml += enc;
    xml += L"</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>";
    xml += safeKey;
    xml += L"</keyMaterial></sharedKey></security></MSM></WLANProfile>";
    Wipe(key);
    Wipe(safeKey);
    {
        std::lock_guard lock(impl->mu); // before the profile exists: windows may start joining the moment it does
        impl->keyRaw = n.raw;
        impl->keyProfile = name;
        impl->badKeyRaw.clear();
    }
    DWORD reason = 0;
    DWORD rc = WlanSetProfile(impl->h, &impl->iface, WLAN_PROFILE_USER, xml.c_str(), nullptr, FALSE, nullptr, &reason); // yours only, never overwriting; windows stores it encrypted
    Wipe(xml);
    if (rc != ERROR_SUCCESS) {
        Log(L"couldn't save the profile");
        std::lock_guard lock(impl->mu);
        impl->keyRaw.clear();
        impl->keyProfile.clear();
        return false;
    }
    WifiNet saved = n;
    saved.profile = name; // connect through the profile just made
    if (Connect(saved)) return true;
    {
        std::lock_guard lock(impl->mu);
        impl->keyRaw.clear();
        impl->keyProfile.clear();
    }
    WlanDeleteProfile(impl->h, &impl->iface, name.c_str(), nullptr); // couldn't even start: leave nothing behind
    return false;
}

std::string Wifi::WrongKey() const {
    if (!impl) return {};
    std::lock_guard lock(impl->mu);
    return impl->badKeyRaw;
}

void Wifi::ForgetWrongKey() {
    if (!impl) return;
    std::lock_guard lock(impl->mu);
    impl->badKeyRaw.clear();
}

void Wifi::Forget(const WifiNet& n) {
    if (!impl || !impl->ready || n.profile.empty()) return;
    if (WlanDeleteProfile(impl->h, &impl->iface, n.profile.c_str(), nullptr) != ERROR_SUCCESS) Log(L"couldn't forget the network");
    impl->Refresh(); // no event says a profile went
    impl->Ping();
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
