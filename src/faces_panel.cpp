// What rises behind a face: more detail, a list to pick from, and the few fields it needs set up.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwchar>

#include "faces.h"
#include "faces_state.h"
#include "feeds.h"
#include "glyphs.h"

namespace faces {
namespace {

constexpr float kHead = 26, kRow = 28, kFieldH = 46, kNoteH = 20;
constexpr int kMaxRows = 8;
constexpr size_t kMaxTyped = 4000;

struct Field {
    const char* key = nullptr;
    const wchar_t* label = nullptr;
    bool secret = false;
    const wchar_t* hint = L"";
};

struct Setup {
    const char* prefix; // meta keys starting with this share these
    const char* feed;
    Field fields[3];
    const wchar_t* note;
};

// a key is only ever a read-only one you made yourself; what each needs, in the words its maker uses
const Setup kSetups[] = {
    {"money.stripe", "stripe", {{"stripe.key", L"Restricted API key", true, L"rk_live_…"}}, L"Dashboard, Developers, API keys: read access to balance"},
    {"money.polar", "polar", {{"polar.key", L"Access token", true, L"polar_oat_…"}}, L"Settings, Developers: metrics read"},
    {"money.dodo", "dodo", {{"dodo.key", L"API key", true, L""}}, L"Developer, API keys"},
    {"money.shopify", "shopify", {{"shopify.id", L"Store domain", false, L"yours.myshopify.com"}, {"shopify.key", L"Admin API token", true, L"shpat_…"}}, L"A custom app with read_orders"},
    {"money.lemon", "lemon", {{"lemon.key", L"API key", true, L""}}, L"Settings, API"},
    {"money.paddle", "paddle", {{"paddle.key", L"API key", true, L"pdl_live_…"}}, L"Developer tools, Authentication: transactions read"},
    {"money.gumroad", "gumroad", {{"gumroad.key", L"Access token", true, L""}}, L"Settings, Advanced, Applications"},
    {"money.total", nullptr, {{"money.sym", L"Currency symbol", false, L"$"}}, L"Adds up every store connected in its own tile"},
    {"web.plausible", "plausible", {{"plausible.id", L"Site", false, L"yoursite.com"}, {"plausible.key", L"API key", true, L""}}, L"Account settings, API keys"},
    {"web.dub", "dub", {{"dub.key", L"API key", true, L"dub_…"}}, L"Workspace settings, API keys"},
    {"web.datafast", "datafast", {{"datafast.key", L"API key", true, L"df_…"}}, L"Website settings, API"},
    {"web.resend", "resend", {{"resend.key", L"API key", true, L"re_…"}}, L"API keys"},
    {"web.linear", "linear", {{"linear.key", L"Personal API key", true, L"lin_api_…"}}, L"Settings, API"},
    {"web.supabase", "supabase", {{"supabase.id", L"Project ref", false, L""}, {"supabase.key", L"Access token", true, L"sbp_…"}}, L"Account, Access tokens"},
    {"web.gmail", "gmail", {{"gmail.id", L"Address", false, L""}, {"gmail.key", L"App password", true, L""}}, L"Google account, Security, App passwords"},
    {"web.githubover", "github.overview", {{"github.key", L"Personal access token", true, L"github_pat_…"}}, L"Notifications and metadata, read only"},
    {"web.githubgrid", "github.activity", {{"github.key", L"Personal access token", true, L"github_pat_…"}}, L"Read only"},
    {"web.github", "github", {{"github.key", L"Personal access token", true, L"github_pat_…"}}, L"Notifications, read only"},
    {"web.leetcode", "leetcode", {{"leetcode.id", L"Username", false, L""}}, L"Your public profile: no key needed"},
    {"social.youtube", "youtube", {{"youtube.id", L"Channel ID", false, L"UC…"}, {"youtube.key", L"API key", true, L""}}, L"Google Cloud, YouTube Data API v3"},
    {"social.x", "x", {{"x.id", L"Username", false, L""}, {"x.key", L"Bearer token", true, L""}}, L"X developer portal"},
    {"social.tiktok", "tiktok", {{"tiktok.key", L"Access token", true, L""}}, L"TikTok for Developers: user.info.stats"},
    {"social.instagram", "instagram", {{"instagram.key", L"Access token", true, L""}}, L"Meta for Developers: Instagram API"},
    {"stock.one", nullptr, {{"stock.id", L"Ticker", false, L"AAPL"}}, L"Public quotes, delayed"},
    {"stock.chart", nullptr, {{"stock.id", L"Ticker", false, L"AAPL"}}, L"Public quotes, delayed"},
    {"stock.watch", nullptr, {{"stock.watch", L"Three tickers", false, L"AAPL,MSFT,NVDA"}}, L"Public quotes, delayed"},
    {"money.metric", "metric", {{"metric.id", L"Pair", false, L"BTC-USD"}}, L"A public price"},
    {"money.currency", "rates", {{"rates.from", L"From", false, L"USD"}, {"rates.to", L"To", false, L"EUR"}}, L"Public daily rates"},
    {"clock.world", nullptr, {{"zones", L"Cities", false, L"London,New York,Tokyo"}}, L"Up to three of the world's big cities"},
    {"clock.worldone", nullptr, {{"zone.one", L"City", false, L"Tokyo"}}, L"One of the world's big cities"},
    {"time.timer", nullptr, {{"timer.min", L"Minutes", false, L"5"}}, nullptr},
    {"list.water", nullptr, {{"water.goal", L"Glasses a day", false, L"8"}}, nullptr},
};

const Setup* SetupFor(const char* key) {
    size_t best = 0;
    const Setup* found = nullptr;
    for (const Setup& s : kSetups) {
        size_t n = strlen(s.prefix);
        if (!strncmp(key, s.prefix, n) && n > best) best = n, found = &s;
    }
    return found;
}

int FieldCount(const Setup* s) {
    int n = 0;
    while (s && n < 3 && s->fields[n].key) ++n;
    return n;
}

struct RowItem {
    std::wstring name, path;
};
std::vector<RowItem> rows; // what the panel lists, read once as it opens
int editing = -1;
std::wstring typed, draft;
int panelKind = -1;

bool Is(int kind, const char* key) { return !strcmp(Info(kind).key, key); }

void Wipe(std::wstring& s) { // a key leaves no copy behind
    if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    s.clear();
}

void ListFolder(const std::wstring& dir, bool imagesOnly) { // the newest few
    struct Found {
        std::wstring name;
        FILETIME at;
    };
    std::vector<Found> all;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY))) continue;
        const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
        if (imagesOnly && !(ext && (!_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg") || !_wcsicmp(ext, L".webp")))) continue;
        all.push_back({fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    size_t keep = (std::min)((size_t)kMaxRows, all.size());
    std::partial_sort(all.begin(), all.begin() + (ptrdiff_t)keep, all.end(), [](const Found& a, const Found& b) { return CompareFileTime(&a.at, &b.at) > 0; });
    for (size_t i = 0; i < keep; ++i) {
        std::wstring shown = all[i].name;
        if (size_t dot = shown.rfind(L".lnk"); dot != std::wstring::npos && dot + 4 == shown.size()) shown.resize(dot); // a recent item's shortcut: its name
        rows.push_back({shown, dir + L"\\" + all[i].name});
    }
}

std::wstring Known(const KNOWNFOLDERID& id, const wchar_t* tail = L"") {
    PWSTR dir = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) out = std::wstring(dir) + tail;
    CoTaskMemFree(dir);
    return out;
}

void Open(const std::wstring& what) { ShellExecuteW(nullptr, L"open", what.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void SetWallpaper(const std::wstring& path) { SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }

void ListWallpapers() {
    ListFolder(Known(FOLDERID_Pictures, L"\\Wallpapers"), true);
    if (rows.empty()) ListFolder(Known(FOLDERID_Pictures), true);
}

void NextWallpaper() {
    rows.clear();
    ListWallpapers();
    if (rows.empty()) return;
    int at = (_wtoi(feeds::Setting("wallpaper.at").c_str()) + 1) % (int)rows.size();
    feeds::SetSetting("wallpaper.at", std::to_wstring(at));
    SetWallpaper(rows[(size_t)at].path);
}

void Save(const Setup* s) {
    if (editing < 0 || !s || editing >= FieldCount(s)) return;
    const Field& f = s->fields[editing];
    if (f.secret) {
        if (!typed.empty()) feeds::SetSecret(f.key, typed); // left empty: the saved key stays
    } else {
        feeds::SetSetting(f.key, typed);
    }
    Wipe(typed);
    if (s->feed) feeds::Refresh(s->feed);
    for (const std::string& id : Feeds({panelKind})) feeds::Refresh(id); // a new ticker is its own feed
    editing = -1;
}

float DetailH(int kind) {
    auto n = [](size_t c) { return (float)(std::max)((size_t)1, (std::min)(c, (size_t)kMaxRows)) * kRow; };
    std::string key = Info(kind).key;
    if (key == "time.screen" || key == "time.timeline") return 86 + kMaxRows * kRow * 0.75f;
    if (key == "time.stopwatch") return 44 + n(state::WatchLaps().size());
    if (key == "time.timer") return 44;
    if (key == "list.notes") return 180;
    if (key == "list.habits") return n(state::Habits().size() + 1) + 8;
    if (key == "list.water") return 44;
    if (key == "sys.bright") return 40;
    if (key == "files.commands" || key == "files.websearch") return kRow + 8;
    if (key == "web.gmail") return n(feeds::Get("gmail").rows.size());
    if (key == "sys.drives" || key == "files.recent" || key == "files.shots" || key == "files.downloads" || key == "files.documents" || key == "files.wallpaper") return n(rows.size());
    return 0;
}

void SearchOrOpen(const std::wstring& what) {
    if (GetFileAttributesW(what.c_str()) != INVALID_FILE_ATTRIBUTES) return Open(what); // a path: open it
    std::wstring q;
    wchar_t b[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, what.c_str(), (int)what.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, what.c_str(), (int)what.size(), u.data(), n, nullptr, nullptr);
    for (unsigned char c : u) {
        if (isalnum(c)) q += (wchar_t)c;
        else swprintf_s(b, L"%%%02X", c), q += b;
    }
    Open(L"https://duckduckgo.com/?q=" + q);
}

}

void OpenPanel(int kind) {
    panelKind = kind;
    editing = -1;
    Wipe(typed);
    draft = Is(kind, "list.notes") ? feeds::Setting("notes") : L"";
    rows.clear();
    std::string key = Info(kind).key;
    if (key == "files.recent") ListFolder(Known(FOLDERID_Recent), false);
    else if (key == "files.shots") ListFolder(Known(FOLDERID_Pictures, L"\\Screenshots"), true);
    else if (key == "files.downloads") ListFolder(Known(FOLDERID_Downloads), false);
    else if (key == "files.documents") ListFolder(Known(FOLDERID_Documents), false);
    else if (key == "files.wallpaper") ListWallpapers();
    else if (key == "sys.drives") {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
            UINT type = GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
            ULARGE_INTEGER freeBytes{}, total{};
            UINT was = SetErrorMode(SEM_FAILCRITICALERRORS); // an empty card reader stays quiet
            bool ok = GetDiskFreeSpaceExW(root, &freeBytes, &total, nullptr) != FALSE;
            SetErrorMode(was);
            if (!ok) continue;
            wchar_t b[64];
            swprintf_s(b, L"%lc:   %.0f GB free of %.0f GB", root[0], freeBytes.QuadPart / 1073741824.0, total.QuadPart / 1073741824.0);
            rows.push_back({b, root});
        }
    }
}

void ClosePanel() {
    if (panelKind >= 0) {
        if (Is(panelKind, "list.notes")) feeds::SetSetting("notes", draft);
        if (editing >= 0) Save(SetupFor(Info(panelKind).key));
    }
    Wipe(typed);
    draft.clear();
    rows.clear();
    panelKind = -1;
    editing = -1;
}

float PanelH(int kind, float) {
    const Setup* s = SetupFor(Info(kind).key);
    return kHead + 10 + DetailH(kind) + FieldCount(s) * kFieldH + (s && s->note ? kNoteH : 0) + 16;
}

bool WantsKeys(int kind) {
    return FieldCount(SetupFor(Info(kind).key)) > 0 || Is(kind, "list.notes") || Is(kind, "list.habits") || Is(kind, "files.commands") || Is(kind, "files.websearch");
}

bool Char(int kind, wchar_t ch) {
    const Setup* s = SetupFor(Info(kind).key);
    std::wstring* target = editing >= 0 ? &typed : (Is(kind, "list.notes") || Is(kind, "list.habits") || Is(kind, "files.commands") || Is(kind, "files.websearch")) ? &draft : nullptr;
    if (ch == VK_ESCAPE) {
        if (editing < 0) return false; // nothing being typed: the host closes the panel
        Wipe(typed);
        editing = -1;
        return true;
    }
    if (!target) return false;
    if (ch == VK_BACK) {
        if (!target->empty()) target->pop_back();
    } else if (ch == VK_RETURN) {
        if (editing >= 0) {
            Save(s);
        } else if (Is(kind, "list.notes")) {
            *target += L'\n';
        } else if (draft.empty()) {
            return true;
        } else if (Is(kind, "list.habits")) {
            state::HabitAdd(draft), draft.clear();
        } else if (Is(kind, "files.commands")) {
            Open(draft), draft.clear();
        } else if (Is(kind, "files.websearch")) {
            SearchOrOpen(draft), draft.clear();
        }
    } else if (ch >= 32 && target->size() < kMaxTyped) {
        *target += ch;
    }
    return true;
}

Act Click(int kind, bool right, const Host& h) {
    const Meta& m = Info(kind);
    std::string key = m.key;
    if (key == "time.timer") {
        if (right) return Act::kPanel;
        state::TimerToggle();
        return Act::kNone;
    }
    if (key == "time.stopwatch") {
        if (right) return Act::kPanel;
        state::WatchToggle();
        return Act::kNone;
    }
    if (key == "list.water") {
        state::WaterAdd(right ? -1 : 1);
        return Act::kNone;
    }
    if (key == "sys.lang") {
        state::NextLanguage();
        return Act::kNone;
    }
    if (key == "sys.theme") {
        state::SetLightTheme(!state::LightTheme());
        return Act::kNone;
    }
    if (key == "sys.tray") return Act::kTray;
    if (key == "sys.cpu" || key == "sys.ram" || key == "sys.temp") {
        Open(L"taskmgr.exe");
        return Act::kNone;
    }
    if (key == "sys.bright") return h.bright >= 0 ? Act::kPanel : Act::kNone;
    if (key == "sys.storage") {
        Open(L"ms-settings:storagesense");
        return Act::kNone;
    }
    if (key == "media.mini" || key == "media.compact") return right ? Act::kNext : Act::kPlay;
    if (key == "media.bar") return right ? Act::kPlay : Act::kNext;
    if (key == "files.wallpaper" && !right) {
        NextWallpaper();
        return Act::kNone;
    }
    if (key == "files.photo") {
        if (right) state::NextPhoto();
        else Open(Known(FOLDERID_Pictures));
        return Act::kNone;
    }
    if (key == "files.emoji") {
        INPUT in[4] = {};
        for (INPUT& i : in) i.type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[3].ki.wVk = VK_LWIN;
        in[1].ki.wVk = in[2].ki.wVk = VK_OEM_PERIOD;
        in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
        return Act::kNone;
    }
    bool hasPanel = FieldCount(SetupFor(m.key)) > 0 || key == "time.screen" || key == "time.timeline" || key == "list.notes" || key == "list.habits" || key == "sys.drives" || key.rfind("files.", 0) == 0;
    return hasPanel ? Act::kPanel : Act::kNone;
}

void Panel(Ui& ui, const Theme& t, int kind, const D2D1_RECT_F& r, const Host& h) {
    Canvas& c = ui.C();
    const Meta& m = Info(kind);
    std::string key = m.key;
    c.Text(m.name, Rect(r.left, r.top - 2, Width(r), 18), Weight::Semi, 13, t.text);
    float y = r.top + kHead;
    bool caret = ((int)(ui.Now() * 2) & 1) == 0;
    auto list = [&](const wchar_t* empty, auto click) {
        if (rows.empty()) {
            c.Text(empty, Rect(r.left, y, Width(r), kRow), Weight::Regular, 12, t.textDim);
            y += kRow;
            return;
        }
        int picked = -1;
        for (size_t i = 0; i < rows.size() && i < (size_t)kMaxRows; ++i, y += kRow) {
            D2D1_RECT_F row = Rect(r.left - 6, y, Width(r) + 12, kRow - 2);
            Ui::Hit hit = ui.Button(Ui::Id("fc.row", (int)i), row);
            if (hit.hover) c.Fill(row, 10, t.text.Alpha(0.07f));
            c.Text(rows[i].name, Inset(row, 10, 0), Weight::Regular, 12, t.text);
            if (hit.clicked) picked = (int)i;
        }
        if (picked >= 0) click(rows[(size_t)picked]);
    };
    auto typedRow = [&](const wchar_t* hint) {
        D2D1_RECT_F f = Rect(r.left - 6, y, Width(r) + 12, kRow + 2);
        c.Fill(f, 10, t.text.Alpha(0.06f));
        if (draft.empty()) c.Text(hint, Inset(f, 12, 0), Weight::Regular, 12, t.textLow);
        c.Text(draft + (caret ? L"│" : L""), Inset(f, 12, 0), Weight::Regular, 12, t.text);
        ui.KeepAlive(); // the caret blinks
        y += kRow + 8;
    };
    if (key == "time.screen" || key == "time.timeline") {
        feeds::Screen s = feeds::ScreenTime();
        c.Text(L"Today " + Hours(s.today) + L"   ·   This week " + Hours(s.week), Rect(r.left, y, Width(r), 16), Weight::Medium, 11.5f, t.textDim);
        y += 22;
        int top = 1;
        for (int v : s.hours) top = (std::max)(top, v);
        float bw = Width(r) / 24;
        for (int i = 0; i < 24; ++i) {
            float bh = (std::max)(2.0f, 44.0f * s.hours[i] / top);
            c.Fill(Rect(r.left + bw * i + 1, y + 44 - bh, bw - 2, bh), 2, s.hours[i] ? t.accentHi : t.text.Alpha(0.08f));
        }
        y += 56;
        float rh = kRow * 0.75f;
        for (size_t i = 0; i < s.apps.size() && i < (size_t)kMaxRows; ++i, y += rh) {
            c.Text(s.apps[i].app, Rect(r.left, y, Width(r) * 0.6f, rh), Weight::Regular, 11.5f, t.text);
            c.Text(Hours(s.apps[i].seconds), Rect(r.left, y, Width(r), rh), Weight::Medium, 11.5f, t.textDim, Align::Right);
        }
    } else if (key == "time.timer") {
        if (w::Chip(ui, t, Ui::Id("fc.timer.go"), Rect(r.left, y, 90, 28), state::TimerRunning() ? L"Pause" : L"Start", state::TimerRunning())) state::TimerToggle();
        if (w::Chip(ui, t, Ui::Id("fc.timer.reset"), Rect(r.left + 98, y, 90, 28), L"Reset", false)) state::TimerReset();
        y += 44;
    } else if (key == "time.stopwatch") {
        if (w::Chip(ui, t, Ui::Id("fc.sw.go"), Rect(r.left, y, 80, 28), state::WatchRunning() ? L"Pause" : L"Start", state::WatchRunning())) state::WatchToggle();
        if (w::Chip(ui, t, Ui::Id("fc.sw.lap"), Rect(r.left + 88, y, 80, 28), L"Lap", false)) state::WatchLap();
        if (w::Chip(ui, t, Ui::Id("fc.sw.reset"), Rect(r.left + 176, y, 80, 28), L"Reset", false)) state::WatchReset();
        y += 44;
        const auto& laps = state::WatchLaps();
        for (size_t i = laps.size(); i-- > 0 && laps.size() - i <= (size_t)kMaxRows; y += kRow) {
            wchar_t b[48];
            swprintf_s(b, L"Lap %zu   %d:%05.2f", i + 1, (int)laps[i] / 60, std::fmod(laps[i], 60.0));
            c.Text(b, Rect(r.left, y, Width(r), kRow), Weight::Regular, 12, t.text);
        }
    } else if (key == "list.notes") {
        D2D1_RECT_F box = Rect(r.left - 4, y, Width(r) + 8, 172);
        c.Fill(box, 12, t.text.Alpha(0.05f));
        std::wstring shown = draft + (caret ? L"│" : L" ");
        ComPtr<IDWriteTextLayout> lay;
        if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), gfx::TextFmt(Weight::Regular, 12.5f, Align::Left), Width(box) - 20, Height(box) - 16, &lay))) {
            ComPtr<ID2D1SolidColorBrush> ink;
            c.Target()->CreateSolidColorBrush(t.text.D2D(), &ink);
            if (ink) c.Target()->DrawTextLayout({box.left + 10, box.top + 8}, lay.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        ui.KeepAlive(); // the caret blinks
        y += 180;
    } else if (key == "list.habits") {
        auto habits = state::Habits();
        int today = state::Today();
        int flip = -1, drop = -1;
        for (size_t i = 0; i < habits.size() && i < (size_t)kMaxRows - 1; ++i, y += kRow) {
            D2D1_RECT_F row = Rect(r.left - 6, y, Width(r) + 12, kRow - 2);
            Ui::Hit hit = ui.Button(Ui::Id("fc.habit", (int)i), row);
            if (hit.hover) c.Fill(row, 10, t.text.Alpha(0.07f));
            bool done = habits[i].last == today;
            D2D1_POINT_2F o = {row.left + 16, (row.top + row.bottom) / 2};
            c.Fill(Rect(o.x - 8, o.y - 8, 16, 16), 5, done ? t.accent : t.text.Alpha(0.1f));
            if (done) c.Icon(glyph::Check, o, 13, t.onAccent);
            c.Text(habits[i].name, Rect(row.left + 32, row.top, Width(row) - 110, Height(row)), Weight::Regular, 12, t.text);
            c.Text(std::to_wstring(habits[i].streak) + L" days", Rect(row.left, row.top, Width(row) - 10, Height(row)), Weight::Semi, 11, t.accentHi, Align::Right);
            if (hit.clicked) flip = (int)i;
            if (ui.In().rclick && Inside(row, ui.In().mouse)) drop = (int)i;
        }
        typedRow(L"Name a new habit and press Enter");
        if (flip >= 0) state::HabitFlip((size_t)flip);
        if (drop >= 0) state::HabitDrop((size_t)drop);
    } else if (key == "list.water") {
        if (w::Chip(ui, t, Ui::Id("fc.water.less"), Rect(r.left, y, 90, 28), L"One less", false)) state::WaterAdd(-1);
        if (w::Chip(ui, t, Ui::Id("fc.water.more"), Rect(r.left + 98, y, 90, 28), L"One more", true)) state::WaterAdd(1);
        y += 44;
    } else if (key == "sys.bright") {
        float v = h.bright / 100.0f;
        D2D1_RECT_F rail = Rect(r.left, y + 10, Width(r), 6);
        constexpr float kThumb = 7;
        if (ui.Slider(Ui::Id("fc.bright"), Inset(rail, kThumb, -10), v, false) && h.setBright) h.setBright(h.ctx, (int)std::lround(v * 100));
        c.Fill(rail, 3, t.text.Alpha(0.14f));
        c.Fill(Rect(rail.left, rail.top, (std::max)(6.0f, Width(rail) * v), 6), 3, t.accentHi);
        c.Dot({w::ThumbAt(rail.left, rail.right, kThumb, v), rail.top + 3}, kThumb, t.text);
        y += 40;
    } else if (key == "files.commands") {
        typedRow(L"A program, a folder or a link, then Enter");
    } else if (key == "files.websearch") {
        typedRow(L"Search the web, or a folder path, then Enter");
    } else if (key == "web.gmail") {
        feeds::Series s = feeds::Get("gmail");
        for (size_t i = 0; i < s.rows.size() && i < (size_t)kMaxRows; ++i, y += kRow) c.Text(s.rows[i], Rect(r.left, y, Width(r), kRow), Weight::Regular, 12, t.text);
    } else if (key == "files.recent" || key == "files.shots" || key == "files.downloads" || key == "files.documents" || key == "sys.drives") {
        list(L"Nothing here yet", [](const RowItem& it) { Open(it.path); });
    } else if (key == "files.wallpaper") {
        list(L"No pictures in Pictures\\Wallpapers or Pictures", [](const RowItem& it) { SetWallpaper(it.path); });
    }

    const Setup* s = SetupFor(m.key);
    int n = FieldCount(s);
    for (int i = 0; i < n; ++i, y += kFieldH) {
        const Field& f = s->fields[i];
        bool on = editing == i;
        c.Text(f.label, Rect(r.left, y, Width(r), 14), Weight::Regular, 11, t.textDim);
        D2D1_RECT_F box = Rect(r.left - 2, y + 15, Width(r) + 4, 26);
        Ui::Hit hit = ui.Button(Ui::Id("fc.field", i), box);
        c.Fill(box, 8, t.text.Alpha(on ? 0.1f : 0.06f));
        if (on) c.Stroke(box, 8, t.accent.Alpha(0.8f), 1);
        D2D1_RECT_F in = Inset(box, 10, 0);
        if (on) {
            if (f.secret) c.TextOnce(std::wstring(typed.size(), L'•') + (caret ? L"│" : L""), in, Weight::Regular, 12, t.text); // never the key itself, never cached
            else c.Text(typed + (caret ? L"│" : L""), in, Weight::Regular, 12, t.text);
            ui.KeepAlive();
        } else if (f.secret) {
            bool set = !feeds::Setting(f.key).empty();
            c.Text(set ? L"Saved. Click to replace it" : f.hint[0] ? f.hint : L"Paste it here", in, Weight::Regular, 12, set ? t.textDim : t.textLow);
        } else {
            std::wstring v = feeds::Setting(f.key);
            c.Text(v.empty() ? std::wstring(f.hint) : v, in, Weight::Regular, 12, v.empty() ? t.textLow : t.text);
        }
        if (hit.clicked && !on) {
            Save(s);
            editing = i;
            typed = f.secret ? L"" : feeds::Setting(f.key);
        }
    }
    if (s && s->note) c.Text(s->note, Rect(r.left, y + 2, Width(r), 16), Weight::Light, 10.5f, t.textLow);
    if (m.feed) {
        feeds::Series got = feeds::Get(m.feed);
        if (!got.ok && !got.error.empty()) c.Text(got.error, Rect(r.left, r.top - 2, Width(r), 18), Weight::Regular, 11, Color::Rgb(0xFF6B6B), Align::Right);
    }
}

}
