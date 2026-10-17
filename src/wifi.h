#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct WifiNet {
    std::wstring ssid; // display name
    std::string raw; // exact ssid bytes, what connect matches on
    std::wstring profile; // saved profile, empty if never joined
    int signal = 0; // 0-100
    bool secured = false, on = false, infra = true; // on = connected, infra = access point (not ad hoc)
    bool pending = false; // connect/disconnect in flight
    int band = 0; // strongest access point's band: 2 (2.4 GHz), 5 or 6, 0 unknown
    int gen = 0, rate = 0; // the connected one only: wi-fi generation (4, 5, 6, 7) and link speed in Mbps
    std::wstring auth; // the connected one only: WPA2, WPA3, Open...
    int authAlgo = 0, cipher = 0; // what it asks for, to build a profile when you join it with a password
};

enum class WifiState { Ok, NoAdapter, Denied }; // Denied: windows hides networks while location access is off

class Wifi { // lazy like Bt: holds nothing until Open, frees all on Close
public:
    Wifi();
    ~Wifi();
    Wifi(const Wifi&) = delete;
    Wifi& operator=(const Wifi&) = delete;
    void Open(HWND hwnd, UINT msg); // opens wlan, listens for scan/connect events, starts a scan
    void Close();
    void Scan();
    bool Scanning() const;
    WifiState State() const;
    void Snap(std::vector<WifiNet>& out) const; // connected first, then saved, then by signal
    bool Connect(const WifiNet& n); // saved or open networks only, false if it needs a password
    bool CanJoin(const WifiNet& n) const; // a new network a password alone gets into (wpa/wpa2/wpa3 personal)
    bool Join(const WifiNet& n, std::wstring& key); // saves a profile with the key and connects; wipes key; a wrong key removes the profile again
    std::string WrongKey() const; // raw ssid of the last join whose key didn't work, empty if none
    void ForgetWrongKey();
    void Forget(const WifiNet& n); // deletes its saved profile; if it's the one we're on, windows drops it too
    void Disconnect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
