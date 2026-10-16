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
