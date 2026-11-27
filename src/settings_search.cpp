#include "settings_search.h"

#include <windows.h>

#include <algorithm>

namespace setsearch {

namespace {

const Card kCards[] = { // in the order the page shows them
    {L"Theme", L"Different theme when not on desktop|Desktop|Not on desktop|Theme|Apps", false},
    {L"Material", L"Dark|Light|Glass|Custom|Clear|Chrome|Starfall|HaloBar|Blur|Opacity|Look|Transparency|Background", false},
    {L"Position", L"Top edge|Left edge|Right edge|Edge|Side|Where it lives", false},
    {L"Screens", L"Pill on every screen|One screen|Every screen|Monitor|Display|All screens", false},
    {L"Glass", L"Dark tint|White tint|Tint color|Clear|Blur", false},
    {L"Notifications", L"Do not disturb|Shown as pills|Pills for HaloBar|Toasts|Alerts|Quiet", false},
    {L"Island", L"Stays|Up whenever music plays|Pill on the desktop|Island pill|How long it stays", false},
    {L"Motion", L"Reduced|Full|Reduce motion|Animations|Calm", false},
    {L"Status pill", L"Always show the pill|Left widget|Right widget|Timer bubble beside the pill|Clock|Widgets", false},
    {L"Long pill", L"Regular|Long|On the desktop|Not on desktop|Customize|Widgets|Bluetooth battery", false},
    {L"Pages and tiles", L"Right-click the rail to edit your pages|Edit the tiles on the Tools page|Rail|Quick tiles|Order", false},
    {L"Live pills", L"Meeting reminders|Join a call|Calendar|Mic and camera in use|Microphone|Camera|Privacy|Downloads|Download progress", false},
    {L"Layout", L"The wide island|Pages side by side|A narrow column|Sidebar|Compact", false},
    {L"Fullscreen dashboard", L"Dashboard on screen|Live tiles|Arrange its tiles|Ctrl+Alt+D", false},
    {L"Media", L"Compact player|Music in the pill|Album art|Art background|Visualizer|Waveform|Lyrics|Cover", false},
    {L"Fullscreen keys", L"Two screens|Swap screens|Lyrics only|Keys|Shortcuts|Fullscreen player", false},
    {L"Panel", L"Pinned open|Reserve space for the pill|Floating card|Pin|Maximized windows", true},
    {L"Power", L"Charging and on battery pills|Low at|Battery|Laptop|Plugged in", true},
    {L"Weather", L"Fahrenheit|Celsius|City|Location|Forecast|Open-Meteo|Temperature", true},
    {L"Volume and brightness", L"Replace Windows volume popup|Volume keys|Brightness|Volume pill|HUD", true},
    {L"Backup", L"Export|Import|Reset to defaults|Hold to reset every setting|Restore", true},
    {L"Display", L"Size|Scale|Zoom|Bigger|Smaller|Text size", true},
    {L"Opening", L"Open only with the shortcut|Win+Alt+A|Hover|Shortcut", true},
    {L"Over apps", L"Open with the shortcut only over apps|Hover delay over apps|Maximized", true},
    {L"Fullscreen", L"Show the wrong lyrics button|Show the edit button|Pencil|Fullscreen player", true},
    {L"Shelf", L"Catch everything you copy|Clipboard history|Keep after restart, encrypted|Clear unpinned at each start|Copy|Paste", true},
    {L"Shelf history", L"Keep up to|Delete after|Cards|Expire|Clipboard", true},
    {L"Fullscreen apps", L"Stay up in fullscreen|Click the pill to open the panel|Games|Videos", true},
    {L"Report a problem", L"Copy report|Open GitHub|Bug|Feedback|Crash|Log", true},
    {L"Accessibility", L"High contrast|Follow Windows text size|Text size|Keyboard navigation|Screen reader support|Narrator|NVDA|JAWS", true},
    {L"Astraeus", L"Check for updates|What's new|Release notes|Automatic updates|Version|Update", false},
    {L"Tour", L"Take the tour|Show it when Astraeus starts|Help|Guide", false},
};
constexpr int kCount = (int)(sizeof(kCards) / sizeof(kCards[0]));
constexpr size_t kMost = 12; // a screenful: past this, more letters say it better
constexpr int kTitleStart = 120, kTitleWord = 90, kTitleIn = 60, kWordStart = 40, kWordIn = 15, kOrderStep = 2; // what kind of match counts for how much

struct Lowered {
    std::wstring title, words;
};

std::wstring Lower(const wchar_t* s) {
    std::wstring out(s);
    if (!out.empty()) CharLowerBuffW(out.data(), (DWORD)out.size());
    return out;
}

const std::vector<Lowered>& Index() { // lowered once, the first time anything's searched
    static const std::vector<Lowered> index = [] {
        std::vector<Lowered> v;
        v.reserve(kCount);
        for (const Card& c : kCards) v.push_back({Lower(c.title), Lower(c.words)});
        return v;
    }();
    return index;
}

bool WordStart(const std::wstring& s, size_t at) { return at == 0 || !iswalnum(s[at - 1]); }

size_t Seek(const std::wstring& in, const std::wstring& token, bool& start) { // the first match, preferring one at a word's start
    size_t first = std::wstring::npos;
    for (size_t at = in.find(token); at != std::wstring::npos; at = in.find(token, at + 1)) {
        if (WordStart(in, at)) {
            start = true;
            return at;
        }
        if (first == std::wstring::npos) first = at;
    }
    start = false;
    return first;
}

}

int Count() { return kCount; }
const Card& At(int i) { return kCards[(std::clamp)(i, 0, kCount - 1)]; }

void Find(const std::wstring& query, std::vector<Hit>& out) {
    out.clear();
    std::wstring q = Lower(query.c_str());
    std::vector<std::wstring> tokens;
    for (size_t a = 0; a < q.size();) {
        size_t e = q.find_first_of(L" \t", a);
        if (e == std::wstring::npos) e = q.size();
        if (e > a) tokens.push_back(q.substr(a, e - a));
        a = e + 1;
    }
    if (tokens.empty()) return;
    const std::vector<Lowered>& index = Index();
    for (int i = 0; i < kCount; ++i) {
        const Lowered& lw = index[(size_t)i];
        int score = 0;
        bool all = true;
        size_t lineAt = std::wstring::npos, lineLen = 0, titleAt = std::wstring::npos;
        for (size_t k = 0; k < tokens.size() && all; ++k) {
            const std::wstring& tk = tokens[k];
            bool start = false;
            size_t t = Seek(lw.title, tk, start);
            if (t != std::wstring::npos) {
                score += t == 0 ? kTitleStart : start ? kTitleWord : kTitleIn;
                if (titleAt == std::wstring::npos) titleAt = t;
                continue;
            }
            size_t w = Seek(lw.words, tk, start);
            if (w == std::wstring::npos) {
                all = false;
                break;
            }
            score += start ? kWordStart : kWordIn;
            if (lineAt == std::wstring::npos) lineAt = w, lineLen = tk.size();
        }
        if (!all) continue;
        const Card& c = kCards[i];
        Hit h;
        h.card = i;
        h.score = score - i / kOrderStep; // a tie keeps the page's order
        std::wstring words = c.words;
        if (lineAt != std::wstring::npos) { // the phrase holding the match, with where it sits in it
            size_t s = words.rfind(L'|', lineAt), e = words.find(L'|', lineAt);
            s = s == std::wstring::npos ? 0 : s + 1;
            h.line = words.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
            h.at = lineAt - s, h.len = lineLen;
        } else { // the title matched: the card's first phrases say what's in it
            size_t e = words.find(L'|');
            size_t e2 = e == std::wstring::npos ? e : words.find(L'|', e + 1);
            h.line = words.substr(0, e2);
            for (wchar_t& ch : h.line)
                if (ch == L'|') ch = L'·';
            h.inTitle = true, h.at = titleAt, h.len = tokens[0].size();
        }
        out.push_back(std::move(h));
    }
    std::stable_sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (out.size() > kMost) out.resize(kMost);
}

}
