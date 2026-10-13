#include <windows.h>
#include <dwmapi.h>

#include "sys.h"

namespace sys {

namespace {

const wchar_t* const kCapHosts[] = {L"SnippingTool.exe", L"ScreenClippingHost.exe"}; // processes that draw the snip overlay
constexpr double kCoverage = 0.9; // overlay must cover 90% of the monitor

bool IsCapHost(const wchar_t* exeName) {
    for (const wchar_t* host : kCapHosts)
        if (_wcsicmp(exeName, host) == 0) return true;
    return false;
}

double Area(const RECT& r) { // 0 for empty/inverted rects
    return r.right > r.left && r.bottom > r.top ? (double)(r.right - r.left) * (r.bottom - r.top) : 0; // double avoids int overflow
}

std::wstring ExeName(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); // limited right works without elevation
    if (!process) return L"";
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH; // in: buffer chars, out: written
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (!ok) return L"";
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

bool IsCapWin(const wchar_t* exeName, const RECT& window, bool topmost, const RECT& monitor) {
    if (!topmost || !IsCapHost(exeName)) return false;
    RECT overlap;
    if (!IntersectRect(&overlap, &window, &monitor)) return false;
    double full = Area(monitor);
    return full > 0 && Area(overlap) >= full * kCoverage;
}

struct Search { // EnumWindows context via lparam
    RECT monitor;
    bool found = false;
};

BOOL CALLBACK Visit(HWND hwnd, LPARAM lp) {
    Search& s = *reinterpret_cast<Search*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE; // TRUE = keep enumerating
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE; // visible but hidden by dwm (other desktop, suspended uwp)
    bool topmost = (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    RECT r;
    if (!topmost || !GetWindowRect(hwnd, &r)) return TRUE; // cheap filter before opening the process
    if (IsCapWin(ExeName(hwnd).c_str(), r, topmost, s.monitor)) {
        s.found = true;
        return FALSE;
    }
    return TRUE;
}

constexpr wchar_t kRegKey[] = L"Software\\Astraeus"; // HKCU settings root

}

bool CaptureOn(const RECT& monitor) { // true while a snip overlay covers this monitor
    Search s{monitor};
    EnumWindows(Visit, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

bool ReadFlag(const wchar_t* name, bool fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) // missing or wrong type
        return fallback;
    return value != 0;
}

void SaveFlag(const wchar_t* name, bool on) {
    DWORD value = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &value, sizeof(value)); // creates the key if missing
}

}
