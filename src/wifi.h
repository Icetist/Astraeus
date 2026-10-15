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
    void Disconnect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
