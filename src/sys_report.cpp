#include "report.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <string>

#include "sys.h"
#include "version.h"

extern "C" LONG NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW); // getversionex lies without a manifest entry

namespace {

constexpr DWORD kLogTail = 256 << 10; // bytes read off the end of the log: all of it, it starts over past this size
constexpr size_t kOtherLines = 20, kStartLines = 5; // crashes and failures, then the plain "started" lines that would bury them
constexpr wchar_t kStartMark[] = L"Start ";
constexpr int kUnset = -1;

// plain numbers only, named one by one: text settings hold places, notes and screen names, and anything not on this list stays out of a report
constexpr const wchar_t* kShared[] = {L"Appearance", L"AccentMode", L"BgOpacity", L"GlassBlur", L"Layout", L"Edge", L"Corners", L"UiScale", L"Screens", L"ReduceMotion", L"Hud",
                                      L"KeyOnly", L"AppKeyOnly", L"DefaultPage", L"RememberPage", L"PillAlways", L"PillDesktop", L"NotifyPills", L"Quiet", L"Board", L"MediaCompact", L"MediaLyrics", L"Fahrenheit",
                                      L"MediaArtBg", L"StageMode", L"StageAnimation", L"StageThump", L"FullLyrics", L"VizFull", L"A11yContrast", L"A11yKeys", L"A11yReader",
                                      L"A11yText", L"A11yFollowText"};

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

bool WordChar(wchar_t c) { return std::iswalnum(c) != 0; }

void ReplaceWord(std::wstring& text, const std::wstring& name, const wchar_t* with) {
    const std::wstring low = Lower(name), mark = with;
    std::wstring hay = Lower(text);
    for (size_t at = hay.find(low); at != std::wstring::npos; at = hay.find(low, at)) {
        size_t end = at + low.size();
        bool whole = (at == 0 || !WordChar(hay[at - 1])) && (end == hay.size() || !WordChar(hay[end])); // a short name mustn't eat the inside of other words
        if (!whole) {
            at = end;
            continue;
        }
        text.replace(at, low.size(), mark);
        hay.replace(at, low.size(), mark);
        at += mark.size();
    }
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring ShortPath(const std::wstring& path) { // the same folder can also be written the 8.3 way
    wchar_t buf[MAX_PATH];
    DWORD n = path.empty() ? 0 : GetShortPathNameW(path.c_str(), buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring LogTail() {
    wchar_t exe[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!len || len >= MAX_PATH) return L"";
    std::wstring path = exe;
    path = path.substr(0, path.find_last_of(L'\\')) + L"\\astraeus.log";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    DWORD want = (DWORD)(std::min)((LONGLONG)kLogTail, size.QuadPart);
    LARGE_INTEGER back{};
    back.QuadPart = size.QuadPart - want;
    std::string bytes(want, '\0');
    DWORD got = 0;
    if (!SetFilePointerEx(f, back, nullptr, FILE_BEGIN) || !ReadFile(f, bytes.data(), want, &got, nullptr)) got = 0;
    CloseHandle(f);
    bytes.resize(got);
    if (back.QuadPart > 0) { // began mid line
        size_t nl = bytes.find('\n');
        bytes.erase(0, nl == std::string::npos ? bytes.size() : nl + 1);
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), text.data(), n);
    return text;
}

std::wstring GraphicsName() {
    DISPLAY_DEVICEW d{};
    d.cb = sizeof(d);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &d, 0); ++i)
        if (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return d.DeviceString;
    return L"unknown";
}

std::wstring Pc() {
    RTL_OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    RtlGetVersion(&os);
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    constexpr ULONGLONG kMb = 1024 * 1024;
    constexpr float kDefaultDpi = 96;
    UINT dpi = GetDpiForSystem();
    std::wstring s = L"Windows: " + std::to_wstring(os.dwMajorVersion) + L"." + std::to_wstring(os.dwMinorVersion) + L"." + std::to_wstring(os.dwBuildNumber) + L"\r\n";
    s += L"Memory: " + std::to_wstring(mem.ullTotalPhys / kMb) + L" MB, " + std::to_wstring(mem.ullAvailPhys / kMb) + L" MB free\r\n";
    s += L"Graphics: " + GraphicsName() + L"\r\n";
    s += L"Screens: " + std::to_wstring(GetSystemMetrics(SM_CMONITORS)) + L", main " + std::to_wstring(GetSystemMetrics(SM_CXSCREEN)) + L"x" + std::to_wstring(GetSystemMetrics(SM_CYSCREEN)) + L" at " +
         std::to_wstring((int)(dpi * 100 / kDefaultDpi + 0.5f)) + L"%\r\n";
    return s;
}

}

namespace sys {

std::wstring Scrub(std::wstring text, std::vector<std::wstring> names) {
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (const auto& name : names)
        if (!name.empty()) ReplaceWord(text, name, L"<private>");
    return text;
}

std::wstring LogPick(const std::wstring& log) {
    std::vector<std::wstring> lines;
    for (size_t at = 0; at < log.size();) {
        size_t nl = log.find(L'\n', at);
        size_t end = nl == std::wstring::npos ? log.size() : nl;
        std::wstring line = log.substr(at, end - at);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        at = end + 1;
    }
    std::vector<bool> keep(lines.size(), false);
    size_t starts = 0, others = 0;
    for (size_t i = lines.size(); i-- > 0;) { // newest first: the limits drop the oldest
        bool start = lines[i].rfind(kStartMark, 0) == 0;
        size_t& count = start ? starts : others;
        if (count < (start ? kStartLines : kOtherLines)) {
            ++count;
            keep[i] = true;
        }
    }
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i)
        if (keep[i]) out += lines[i] + L"\r\n";
    return out;
}

std::wstring ProblemReport() {
    std::wstring s = L"Astraeus problem report\r\nVersion: " APP_VER_W L"\r\n" + Pc() + L"\r\nSettings:\r\n";
    for (const wchar_t* name : kShared) {
        int v = ReadNum(name, kUnset);
        if (v != kUnset) s += std::wstring(L"  ") + name + L" = " + std::to_wstring(v) + L"\r\n";
    }
    std::wstring log = LogPick(LogTail());
    s +=L"\r\nRecent log:\r\n" + (log.empty() ? std::wstring(L"  (empty)\r\n") : log);
    std::wstring home = Env(L"USERPROFILE");
    return Scrub(std::move(s), {home, ShortPath(home), Env(L"USERNAME"), Env(L"COMPUTERNAME")});
}

bool CopyText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem); // on success the clipboard owns it
    }
    CloseClipboard();
    return ok;
}

}
