#include "sys.h"

#include <cwctype>

#include <map>

namespace sys {

namespace {
std::map<std::wstring, std::wstring> idOf; // asking windows is the slow part of a once-a-second look: kept until the displays change

const std::wstring& IdOf(const std::wstring& device) { // the monitor itself, by make, model and port: \\.\DISPLAY2 can name another one after a replug
    auto it = idOf.find(device);
    if (it != idOf.end()) return it->second;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    bool got = EnumDisplayDevicesW(device.c_str(), 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0];
    return idOf[device] = got ? dd.DeviceID : device; // a driver that won't say: the device name is all there is
}
}

void ForgetScreenIds() { idOf.clear(); }

HMONITOR FindScreenId(const std::wstring& id) {
    for (const Screen& s : ListScreens())
        if (s.id == id) return FindScreen(s.name);
    return nullptr;
}

std::vector<Screen> ListScreens() {
    std::vector<Screen> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(m, &mi)) return TRUE;
        Screen s;
        s.name = mi.szDevice;
        s.id = IdOf(s.name);
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

float Brightness(const RECT& r) {
    constexpr int kW = 24, kH = 12; // a thumbnail is plenty for "is this a white page"
    if (r.right <= r.left || r.bottom <= r.top) return 0;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kW, -kH, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    float lum = 0;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        SetStretchBltMode(mem, HALFTONE); // averages instead of picking single pixels
        StretchBlt(mem, 0, 0, kW, kH, screen, r.left, r.top, r.right - r.left, r.bottom - r.top, SRCCOPY);
        GdiFlush();
        const auto* px = static_cast<const BYTE*>(bits);
        double sum = 0;
        for (int i = 0; i < kW * kH; ++i) sum += 0.0722 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.2126 * px[i * 4 + 2]; // bgr
        lum = (float)(sum / (kW * kH) / 255.0);
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return lum;
}

}
