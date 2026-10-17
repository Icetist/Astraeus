#include "config.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace {

const char* kDefault = // written on first run, utf-8 ini
    "# Astraeus config. Right-click the tray icon > Restart after editing.\n"
    "# Sizes are in DIPs (pixels at 100% scaling).\n"
    "\n"
    "# --- Dropdown ---------------------------------------------------------\n"
    "PanelWidth = 900\n"
    "PanelHeight = 340\n"
    "Fillet = 26\n"
    "CornerRadius = 34\n"
    "HoverOpen = true\n"
    "HoverDelayMs = 70\n"
    "# Width of the hover zone at the top of the screen, centered\n"
    "HoverWidth = 240\n"
    "CloseDelayMs = 450\n"
    "HideOnFullscreen = true\n"
    "# Free GPU memory this many seconds after the dropdown closes\n"
    "ReleaseGpuAfterSec = 8\n"
    "# Write render timings to perf.log next to the exe\n"
    "PerfLog = false\n"
    "\n"
    "# --- Look ---------------------------------------------------------------\n"
    "# wallpaper = colours from your wallpaper, or a hex colour like #B8C4FF\n"
    "Theme = wallpaper\n"
    "AlbumColors = true\n"
    "# The gradient behind the player pulses with the music (only while open and playing)\n"
    "Visualizer = true\n"
    "\n"
    "# --- Tools --------------------------------------------------------------\n"
    "Weather = true\n"
    "Fahrenheit = false\n"
    "\n"
    "# Download and install new versions automatically\n"
    "AutoUpdate = true\n";

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"); // \r too, handles crlf files
    size_t b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1); // all whitespace -> empty
}

std::wstring Lower(std::wstring s) { // by value, returns modified copy
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

bool ParseBool(const std::wstring& v, bool& out) {
    std::wstring l = Lower(v); // case-insensitive, accepts common synonyms
    if (l == L"true" || l == L"yes" || l == L"on" || l == L"1") { out = true; return true; }
    if (l == L"false" || l == L"no" || l == L"off" || l == L"0") { out = false; return true; }
    return false; // out untouched, default kept
}

bool ParseFloat(const std::wstring& v, float lo, float hi, float& out) {
    wchar_t* end = nullptr;
    double d = wcstod(v.c_str(), &end);
    if (end == v.c_str()) return false; // nothing parsed
    out = (float)(std::max)((double)lo, (std::min)((double)hi, d)); // clamp, out of range is not an error
    return true;
}

bool ParseInt(const std::wstring& v, int lo, int hi, int& out) {
    float f; // reuse float parse so "70.0" also works
    if (!ParseFloat(v, (float)lo, (float)hi, f)) return false;
    out = (int)f; // truncates
    return true;
}

bool ParseColor(const std::wstring& v, Color& out) {
    if (v.size() != 7 || v[0] != L'#') return false; // only #RRGGBB
    wchar_t* end = nullptr;
    unsigned long rgb = wcstoul(v.c_str() + 1, &end, 16);
    if (*end != 0) return false; // reject trailing non-hex chars
    out = Color::Rgb((uint32_t)rgb);
    return true;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0); // first call: measure
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n); // second call: convert
    return w;
}

}

bool ParseHotkey(const std::wstring& val, UINT& mods, UINT& key) { // "Win+Alt+A", "Ctrl+Shift+Space", "off"
    std::wstring v = Lower(val);
    if (v == L"off" || v == L"none" || v.empty()) {
        mods = key = 0;
        return true;
    }
    UINT m = 0, k = 0;
    for (size_t pos = 0; pos <= v.size();) {
        size_t plus = v.find(L'+', pos);
        if (plus == std::wstring::npos) plus = v.size();
        std::wstring part = Trim(v.substr(pos, plus - pos));
        pos = plus + 1;
        if (part == L"win") m |= MOD_WIN;
        else if (part == L"alt") m |= MOD_ALT;
        else if (part == L"ctrl" || part == L"control") m |= MOD_CONTROL;
        else if (part == L"shift") m |= MOD_SHIFT;
        else if (part == L"space") k = VK_SPACE;
        else if (part.size() == 1 && ((part[0] >= L'a' && part[0] <= L'z') || (part[0] >= L'0' && part[0] <= L'9'))) k = (UINT)towupper(part[0]);
        else if (part.size() >= 2 && part[0] == L'f' && _wtoi(part.c_str() + 1) >= 1 && _wtoi(part.c_str() + 1) <= 12) k = VK_F1 + _wtoi(part.c_str() + 1) - 1;
        else return false;
    }
    if (!k || !m) return false; // a bare key would steal it from every app
    mods = m;
    key = k;
    return true;
}

Config Config::Load(const std::wstring& path) {
    Config c;
    std::ifstream in(path, std::ios::binary);
    if (!in) { // missing file: write template, run on defaults
        std::ofstream out(path, std::ios::binary);
        if (out) out << kDefault;
        else c.warns.push_back(L"Could not write default config");
        return c;
    }
    std::stringstream buffer;
    buffer << in.rdbuf(); // slurp whole file
    std::wstringstream lines(Widen(buffer.str()));
    std::wstring raw;
    while (std::getline(lines, raw)) {
        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';' || line[0] == L'[') continue; // comments and [sections] ignored
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue; // not key=value
        std::wstring key = Lower(Trim(line.substr(0, eq))); // keys case-insensitive
        std::wstring val = Trim(line.substr(eq + 1));
        bool ok = true;
        if (key == L"panelwidth") ok = ParseFloat(val, 900, 3000, c.panW); // min = default, panel can't shrink below layout
        else if (key == L"panelheight") ok = ParseFloat(val, 340, 1500, c.panH);
        else if (key == L"fillet") ok = ParseFloat(val, 0, 120, c.fillet);
        else if (key == L"cornerradius") ok = ParseFloat(val, 0, 120, c.corner);
        else if (key == L"hoveropen") ok = ParseBool(val, c.hoverOpen);
        else if (key == L"hoverdelayms") ok = ParseInt(val, 0, 5000, c.hoverMs);
        else if (key == L"hoverwidth") ok = ParseFloat(val, 40, 3000, c.hoverW);
        else if (key == L"closedelayms") ok = ParseInt(val, 0, 10000, c.closeMs);
        else if (key == L"hideonfullscreen") ok = ParseBool(val, c.hideFull);
        else if (key == L"releasegpuaftersec") ok = ParseInt(val, 0, 3600, c.gpuIdleSec);
        else if (key == L"perflog") ok = ParseBool(val, c.perfLog);
        else if (key == L"hotkey") ok = ParseHotkey(val, c.hotMods, c.hotKey);
        else if (key == L"theme") {
            c.wallTheme = Lower(val) == L"wallpaper";
            if (!c.wallTheme) ok = ParseColor(val, c.themeCol); // anything else must be a hex color
        }
        else if (key == L"albumcolors") ok = ParseBool(val, c.albumCol);
        else if (key == L"visualizer") ok = ParseBool(val, c.viz);
        else if (key == L"weather") ok = ParseBool(val, c.weather);
        else if (key == L"fahrenheit") ok = ParseBool(val, c.fahr);
        else if (key == L"autoupdate") ok = ParseBool(val, c.autoUpdate);
        else c.warns.push_back(L"Unknown config key: " + key); // typo guard, keep parsing
        if (!ok) c.warns.push_back(L"Invalid value for " + key + L": " + val); // bad value keeps default
    }
    return c; // last duplicate key wins
}
