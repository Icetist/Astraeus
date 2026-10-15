#include "sys.h"

#include <cwctype>

namespace sys {

std::vector<Screen> ListScreens() {
    std::vector<Screen> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(m, &mi)) return TRUE;
        Screen s;
        s.name = mi.szDevice;
        s.rc = mi.rcMonitor;
        s.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        size_t i = s.name.size(); // windows' own number, the one display settings shows: \\.\DISPLAY2 -> 2
        while (i > 0 && std::iswdigit(s.name[i - 1])) --i;
        s.number = i < s.name.size() ? _wtoi(s.name.c_str() + i) : 0;
        reinterpret_cast<std::vector<Screen>*>(lp)->push_back(s);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

HMONITOR FindScreen(const std::wstring& name) {
    if (name.empty()) return nullptr;
    struct Find {
        const std::wstring* name;
        HMONITOR hit;
    } f{&name, nullptr};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* f = reinterpret_cast<Find*>(lp);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi) && *f->name == mi.szDevice) {
            f->hit = m;
            return FALSE; // found it, stop
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.hit;
}

}
