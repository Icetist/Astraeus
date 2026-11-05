#include "faces_state.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>

#include "feeds.h"
#include "glyphs.h"

#pragma comment(lib, "uuid.lib")

namespace faces::state {
namespace {

constexpr double kDefaultTimerMin = 5;
constexpr int kDefaultGoal = 8;
constexpr UINT kPhotoPx = 360;
constexpr size_t kMaxPhotos = 400;

struct City {
    const wchar_t* name;
    const wchar_t* key; // windows' time zone name
};
const City kCities[] = {
    {L"London", L"GMT Standard Time"}, {L"Paris", L"Romance Standard Time"}, {L"Berlin", L"W. Europe Standard Time"}, {L"Moscow", L"Russian Standard Time"},
    {L"Dubai", L"Arabian Standard Time"}, {L"Delhi", L"India Standard Time"}, {L"Bangkok", L"SE Asia Standard Time"},
    {L"Singapore", L"Singapore Standard Time"}, {L"Beijing", L"China Standard Time"}, {L"Tokyo", L"Tokyo Standard Time"}, {L"Sydney", L"AUS Eastern Standard Time"},
    {L"Auckland", L"New Zealand Standard Time"}, {L"Honolulu", L"Hawaiian Standard Time"}, {L"Los Angeles", L"Pacific Standard Time"}, {L"Denver", L"Mountain Standard Time"},
    {L"Chicago", L"Central Standard Time"}, {L"New York", L"Eastern Standard Time"}, {L"Sao Paulo", L"E. South America Standard Time"}, {L"Johannesburg", L"South Africa Standard Time"},
    {L"Cairo", L"Egypt Standard Time"}, {L"Istanbul", L"Turkey Standard Time"}, {L"Karachi", L"Pakistan Standard Time"}, {L"UTC", L"UTC"},
};

const DYNAMIC_TIME_ZONE_INFORMATION* ZoneOf(const std::wstring& city) { // looked up once per city: windows walks its whole list for each
    static std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION> cache;
    auto it = cache.find(city);
    if (it != cache.end()) return &it->second;
    const wchar_t* key = nullptr;
    for (const City& c : kCities)
        if (!_wcsicmp(c.name, city.c_str())) key = c.key;
    if (!key) return nullptr;
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &tz) == ERROR_SUCCESS; ++i)
        if (!wcscmp(tz.TimeZoneKeyName, key)) return &(cache[city] = tz);
    return nullptr;
}

double Tick() { return (double)GetTickCount64() / 1000; }

double timerEnd = 0, timerHeld = -1; // running: ends at timerEnd; paused: timerHeld seconds left
double watchFrom = 0, watchHeld = 0; // running: since watchFrom, plus what was held
bool watchOn = false;
std::vector<double> laps;

ComPtr<ID2D1Bitmap> photo;
ID2D1RenderTarget* photoOwner = nullptr;
ComPtr<IWICBitmapSource> photoSrc;
bool photoLooked = false;

void LoadPhoto() {
    photoSrc.Reset();
    photo.Reset();
    PWSTR dir = nullptr;
    std::vector<std::wstring> found;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &dir))) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((std::wstring(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
                if (ext && (!_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg") || !_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".webp"))) found.push_back(std::wstring(dir) + L"\\" + fd.cFileName);
            } while (FindNextFileW(f, &fd) && found.size() < kMaxPhotos);
            FindClose(f);
        }
    }
    CoTaskMemFree(dir);
    if (found.empty()) return;
    photoSrc = gfx::DecodeFile(found[GetTickCount64() % found.size()].c_str(), kPhotoPx);
}

void SaveHabits(const std::vector<Habit>& list) {
    std::wstring all;
    for (const Habit& h : list) all += h.name + L"\t" + std::to_wstring(h.last) + L"\t" + std::to_wstring(h.streak) + L"\n";
    feeds::SetSetting("habits", all);
}

int Yesterday() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart -= 864000000000ULL;
    ft = {u.LowPart, u.HighPart};
    FileTimeToSystemTime(&ft, &st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

constexpr wchar_t kThemeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

}

std::vector<std::wstring> Zones(size_t most) {
    std::wstring all = feeds::Setting("zones");
    if (all.empty()) all = L"London,New York,Tokyo";
    std::vector<std::wstring> out;
    for (size_t a = 0; a < all.size() && out.size() < most;) {
        size_t e = (std::min)(all.find(L',', a), all.size());
        std::wstring one = all.substr(a, e - a);
        while (!one.empty() && one[0] == L' ') one.erase(0, 1);
        if (ZoneOf(one)) out.push_back(one);
        a = e + 1;
    }
    return out;
}

SYSTEMTIME ZoneNow(const std::wstring& city) {
    SYSTEMTIME utc, out;
    GetSystemTime(&utc);
    const DYNAMIC_TIME_ZONE_INFORMATION* tz = ZoneOf(city);
    if (!tz || !SystemTimeToTzSpecificLocalTimeEx(tz, &utc, &out)) GetLocalTime(&out);
    return out;
}

std::wstring Offset(const std::wstring& city) {
    SYSTEMTIME z = ZoneNow(city), here;
    GetLocalTime(&here);
    int mins = (z.wHour * 60 + z.wMinute) - (here.wHour * 60 + here.wMinute);
    if (z.wDay != here.wDay) mins += (z.wYear > here.wYear || z.wMonth > here.wMonth || z.wDay > here.wDay) ? 1440 : -1440;
    if (!mins) return L"same";
    wchar_t b[16];
    if (mins % 60) swprintf_s(b, L"%+.1fh", mins / 60.0);
    else swprintf_s(b, L"%+dh", mins / 60);
    return b;
}

double TimerLength() {
    double m = _wtof(feeds::Setting("timer.min").c_str());
    return (m > 0 ? m : kDefaultTimerMin) * 60;
}

bool TimerRunning() { return timerEnd > 0; }

double TimerLeft() {
    TimerCheck();
    if (timerEnd > 0) return (std::max)(0.0, timerEnd - Tick());
    return timerHeld >= 0 ? timerHeld : TimerLength();
}

void TimerToggle() {
    if (timerEnd > 0) timerHeld = (std::max)(0.0, timerEnd - Tick()), timerEnd = 0;
    else timerEnd = Tick() + (timerHeld > 0 ? timerHeld : TimerLength()), timerHeld = -1;
}

void TimerReset() { timerEnd = 0, timerHeld = -1; }

void TimerCheck() {
    if (timerEnd > 0 && Tick() >= timerEnd) {
        timerEnd = 0, timerHeld = -1;
        MessageBeep(MB_ICONASTERISK);
    }
}

double WatchSeconds() { return watchHeld + (watchOn ? Tick() - watchFrom : 0); }
bool WatchRunning() { return watchOn; }

void WatchToggle() {
    if (watchOn) watchHeld += Tick() - watchFrom, watchOn = false;
    else watchFrom = Tick(), watchOn = true;
}

void WatchReset() { watchOn = false, watchHeld = 0, laps.clear(); }

void WatchLap() {
    if (WatchSeconds() > 0) laps.push_back(WatchSeconds());
}

const std::vector<double>& WatchLaps() { return laps; }

int Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

int Water() { return _wtoi(feeds::Setting("water.day").c_str()) == Today() ? _wtoi(feeds::Setting("water.count").c_str()) : 0; }

int WaterGoal() {
    int g = _wtoi(feeds::Setting("water.goal").c_str());
    return g > 0 ? g : kDefaultGoal;
}

void WaterAdd(int by) {
    int next = (std::clamp)(Water() + by, 0, 99);
    feeds::SetSetting("water.count", std::to_wstring(next));
    feeds::SetSetting("water.day", std::to_wstring(Today()));
}

std::vector<Habit> Habits() {
    std::vector<Habit> out;
    std::wstring all = feeds::Setting("habits"); // name, last day and streak, a tab between, a line each
    for (size_t a = 0; a < all.size();) {
        size_t e = (std::min)(all.find(L'\n', a), all.size());
        std::wstring line = all.substr(a, e - a);
        size_t p = line.find(L'\t'), q = p == std::wstring::npos ? p : line.find(L'\t', p + 1);
        if (q != std::wstring::npos) out.push_back({line.substr(0, p), _wtoi(line.c_str() + p + 1), _wtoi(line.c_str() + q + 1)});
        a = e + 1;
    }
    return out;
}

void HabitAdd(const std::wstring& name) {
    auto list = Habits();
    list.push_back({name, 0, 0});
    SaveHabits(list);
}

void HabitFlip(size_t at) {
    auto list = Habits();
    if (at >= list.size()) return;
    Habit& h = list[at];
    int today = Today(), prev = Yesterday();
    if (h.last == today) h.streak = (std::max)(0, h.streak - 1), h.last = h.streak ? prev : 0;
    else h.streak = (h.last == prev ? h.streak : 0) + 1, h.last = today;
    SaveHabits(list);
}

void HabitDrop(size_t at) {
    auto list = Habits();
    if (at < list.size()) list.erase(list.begin() + (ptrdiff_t)at), SaveHabits(list);
}

std::wstring Language() {
    HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (!LCIDToLocaleName(MAKELCID(LOWORD((UINT_PTR)layout), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0)) return L"--";
    std::wstring s = name;
    return s.substr(0, s.find(L'-'));
}

void NextLanguage() {
    if (HWND w = GetForegroundWindow()) PostMessageW(w, WM_INPUTLANGCHANGEREQUEST, INPUTLANGCHANGE_FORWARD, 0);
}

bool LightTheme() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kThemeKey, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

void SetLightTheme(bool light) {
    DWORD v = light ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kThemeKey, L"AppsUseLightTheme", REG_DWORD, &v, sizeof(v));
    RegSetKeyValueW(HKEY_CURRENT_USER, kThemeKey, L"SystemUsesLightTheme", REG_DWORD, &v, sizeof(v));
    std::thread([] { SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 200, nullptr); }).detach(); // every window hears it: never on the ui thread
}

void DrawPhoto(Canvas& c, const Theme& t, const D2D1_RECT_F& r) {
    if (!photoLooked) photoLooked = true, LoadPhoto();
    ID2D1RenderTarget* rt = c.Target();
    if (photoOwner != rt) photo.Reset(), photoOwner = rt;
    if (!photo && photoSrc) rt->CreateBitmapFromWicBitmap(photoSrc.Get(), nullptr, &photo);
    if (photo) {
        c.Image(photo.Get(), Inset(r, 3, 3), 8, 1);
    } else {
        c.Fill(Inset(r, 3, 3), 8, t.text.Alpha(0.06f));
        c.Icon(glyph::Pc, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, 18, t.textDim);
    }
}

void NextPhoto() { LoadPhoto(); }

}
