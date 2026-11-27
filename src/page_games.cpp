// The games page: the game being played or the last one with how it went, the games found on this pc with the time spent in
// each, and how detection, its pills and its sounds behave. Its settings live here, not in astraeus' settings: they're about this page.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <map>
#include <unordered_set>

#include "cues.h"
#include "glyphs.h"
#include "history.h"
#include "page_sheet.h"
#include "stageprefs.h"

using namespace sheet;

namespace {

constexpr float kGap = 10;
constexpr float kNowH = 100, kArtBig = 64, kArtBigR = 14; // the card on top: the game now, or the last one
constexpr float kRowH2 = 46, kArtRow = 30, kArtRowR = 8; // a game in the list
constexpr float kLine = 30; // a switch row
constexpr float kSegH = 26;
constexpr float kBtnH = 28, kBtnW = 108;
constexpr float kListHead = 34; // a list's title line and its buttons
constexpr float kSideW = 340; // the settings column beside the list, when the panel is wide enough
constexpr float kTwoCols = 720; // and this wide it is
constexpr float kPlay = 26; // a sound's preview button
constexpr int kTopPlayed = 5; // most played, shown above the library
constexpr UINT kCoverPx = 192; // launcher art decoded this small: it's drawn at 64 at most
constexpr size_t kArtCap = 128; // pictures kept per device, cleared when it fills
constexpr int kIconPx = 32;
constexpr float kInfoR = 7, kTipPad = 12, kTipLine = 18, kTipSlack = 8; // the ⓘ by the sounds, and its tip
const wchar_t* const kInfoLines[] = {L"Some fullscreen games cover the pill.", L"The sound tells you Astraeus has noticed the game."};
const wchar_t* const kPauseLines[] = {L"A shortcut that stops recording, and starts it again.", L"While paused nothing is kept: a clip after it starts there."};
const wchar_t* const kLongLines[] = {L"Saves the last 2 minutes, whatever the usual length is.", L"Once it's set, 2 minutes are kept in memory while you play."};
const wchar_t* const kAfterLines[] = {L"Keeps recording this long after you press the key,", L"so the clip has the moment and what came of it."};
constexpr float kSortH = 22, kSortText = 11, kSortGap = 14; // the list's order: three quiet words under its title
constexpr float kMenuW = 184, kMenuRow = 30, kMenuPad = 5; // a game's right-click menu
constexpr float kRowBtns = 80; // a library row's rename and not-a-game buttons
constexpr float kHoldSec = 0.8f, kHoldW = 132; // removing a hidden game for good: held, there's no undo

float scrollMain = 0, scrollSide = 0;
History hist; // the page's settings, undone and redone together
bool adding = false; // the list shows programs with a window, to pick one that's a game
struct Running {
    std::wstring exe, name;
};
std::vector<Running> running;
int visitSeen = -1;
D2D1_POINT_2F tipAt{};
float tipFade = 0;
const wchar_t* const* tipLines = kInfoLines; // the hovered i's two lines
bool showHidden = false; // the hidden ones listed under the library
std::wstring menuFor; // the game whose right-click menu is open, empty: none
D2D1_POINT_2F menuAt{};
double menuSince = -1; // the frame it opened on: the click that opened it doesn't close it
int artSeen = -1; // the covers' epoch the pictures were made from
std::vector<sys::games::Game> hiddenList; // read again when the count changes: naming one can read its program's file

struct Pref {
    int Model::*field;
    const wchar_t* key;
    int def;
};
const Pref kPrefs[] = {{&Model::gamePill, L"GamePill", 1}, {&Model::gameSummary, L"GameSummary", 1}, {&Model::gameQuiet, L"GameQuiet", 0},
                       {&Model::cueStyle, L"CueStyle", 0}, {&Model::cueVol, L"CueVolume", sys::cues::kVolDef}, {&Model::cueOn, L"CueGameOn", 1}, {&Model::cueOff, L"CueGameOff", 1},
                       {&Model::cueAlways, L"CueAlways", 1}, {&Model::clipSecs, L"ClipSeconds", 30}, {&Model::clipQuality, L"ClipQuality", 1},
                       {&Model::clipAudio, L"ClipAudio", 1}, {&Model::clipCursor, L"ClipCursor", 0},
                       {&Model::clipFps, L"ClipFps", 60}, {&Model::clipHeight, L"ClipHeight", 0}, {&Model::clipMic, L"ClipMic", 0}, {&Model::clipAfter, L"ClipAfter", 0}}; // not the master switch: reset never turns recording off
constexpr float kNoteH = 34; // two short lines under the clip settings
constexpr float kNoticeW = 600, kNoticeH = 300, kNoticeBtnW = 170, kMarkR = 18; // the page while recording is off
constexpr float kClipBtnW = 104; // save clip, on the card on top
constexpr float kChipW = 128; // the clip key's capture chip
constexpr double kMbitAt1080[] = {6, 10, 16}; // light, balanced, high at 1080p60: for the memory estimate before recording
constexpr double kAudioMbPerSec = 0.19; // the sound kept as plain samples

History::Snap Snap(const Model& m) {
    History::Snap s;
    for (const Pref& p : kPrefs) s.push_back((float)(m.*p.field));
    return s;
}

History::Snap Defaults() {
    History::Snap s;
    for (const Pref& p : kPrefs) s.push_back((float)p.def);
    return s;
}

void Load(Model& m, const History::Snap& s) {
    for (size_t i = 0; i < std::size(kPrefs) && i < s.size(); ++i)
        if (m.*kPrefs[i].field != (int)s[i]) m.SetPref(m.*kPrefs[i].field, kPrefs[i].key, (int)s[i]);
}

bool SwitchRow(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* label, bool on, bool enabled = true) { // label left, switch right; the words flip it too
    Canvas& c = ui.C();
    Ui::Hit h = enabled ? ui.Button(Ui::Id(id, 1), {r.left, r.top, r.right - 52, r.bottom}) : Ui::Hit{};
    if (enabled) ui.NoStop();
    c.Text(label, {r.left, r.top, r.right - 56, r.bottom}, Weight::Regular, 11.5f, enabled ? t.text : t.textLow);
    bool flip = w::Switch(ui, t, Ui::Id(id), {r.right - 24, (r.top + r.bottom) / 2}, on, enabled);
    if (enabled) ui.Name(label);
    return flip || h.clicked;
}

void Flip(Model& m, int& field, const wchar_t* key) { m.SetPref(field, key, field ? 0 : 1); }

std::shared_ptr<std::vector<uint32_t>> IconPixels(const std::wstring& exe) { // the program's own icon, 32 px premultiplied bgra
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(exe.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kIconPx, -kIconPx, 1, 32, BI_RGB}; // top-down rows
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    std::shared_ptr<std::vector<uint32_t>> out;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        DrawIconEx(dc, 0, 0, sfi.hIcon, kIconPx, kIconPx, 0, nullptr, DI_NORMAL); // into a zeroed dib: comes out premultiplied
        GdiFlush();
        out = std::make_shared<std::vector<uint32_t>>(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + kIconPx * kIconPx);
        bool alpha = std::any_of(out->begin(), out->end(), [](uint32_t px) { return (px >> 24) != 0; });
        if (!alpha) // an old icon without alpha: what was drawn is opaque
            for (uint32_t& px : *out)
                if (px & 0xFFFFFF) px |= 0xFF000000;
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    if (dc) DeleteDC(dc);
    DestroyIcon(sfi.hIcon);
    return out;
}

std::wstring When(int64_t start) { // "today 14:32", "yesterday 21:05", or the date
    time_t t = (time_t)start, nowT = time(nullptr);
    tm at{}, now{};
    localtime_s(&at, &t), localtime_s(&now, &nowT);
    wchar_t buf[48];
    int days = (now.tm_year - at.tm_year) * 366 + now.tm_yday - at.tm_yday;
    if (days == 0) swprintf_s(buf, L"today %02d:%02d", at.tm_hour, at.tm_min);
    else if (days == 1) swprintf_s(buf, L"yesterday %02d:%02d", at.tm_hour, at.tm_min);
    else wcsftime(buf, 48, L"%d %b", &at);
    return buf;
}

void Art(Canvas& c, const Theme& t, const D2D1_RECT_F& r, float radius, const std::wstring& cover, const std::wstring& exe) { // the game's picture, or a pad where it has none
    if (ID2D1Bitmap* bmp = GameArt(c, cover, exe)) {
        c.Image(bmp, r, radius);
        return;
    }
    c.Fill(r, radius, t.track);
    c.Icon(glyph::Pad, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, Width(r) * 0.5f, t.textDim);
}

void NowCard(PageCtx& p, const D2D1_RECT_F& r) {
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    c.Fill(r, 16, CardFill(t));
    bool live = m.games.Playing();
    if (!live && !m.games.HasLast()) {
        c.Icon(glyph::Pad, {r.left + 48, (r.top + r.bottom) / 2}, 34, t.textLow);
        c.Text(m.clipOn ? L"No game played yet" : L"Game detection is off", Rect(r.left + 84, r.top + 30, Width(r) - 100, 20), Weight::Medium, 14, t.text);
        c.Text(m.clipOn ? L"Start one: it shows up here, with how long you play" : L"Switch it on below to see what you play", Rect(r.left + 84, r.top + 52, Width(r) - 100, 18), Weight::Light, 12, t.textDim);
        return;
    }
    const GameLog::Session& s = live ? m.games.Now() : m.games.Last();
    float cy = (r.top + r.bottom) / 2;
    D2D1_RECT_F art = Rect(r.left + 18, cy - kArtBig / 2, kArtBig, kArtBig);
    Art(c, t, art, kArtBigR, s.cover, s.exe);
    bool rec = live && m.clip;
    size_t kept = s.clips > 0 ? ClipCount(m, s.name) : 0; // what's in its folder now: clips deleted since don't count
    bool clips = !live && kept > 0;
    float x = art.right + 16, tw = r.right - x - 16 - (rec ? 2 * kClipBtnW + 20 : clips ? kClipBtnW + 12 : 0);
    c.TextSp(rec ? (m.clip->Paused() ? L"PLAYING NOW  ·  PAUSED" : L"PLAYING NOW  ·  RECORDING") : live ? L"PLAYING NOW" : L"LAST PLAYED", Rect(x, cy - 36, tw, 14), Weight::Medium, 10, live ? t.accent : t.textLow, 1.4f);
    D2D1_RECT_F btn = Rect(r.right - kClipBtnW - 16, cy - kBtnH / 2, kClipBtnW, kBtnH);
    if (rec && Act(p.ui, t, Ui::Id("gm.clip.save"), btn, L"Save clip", true, false, !m.clip->Paused())) m.ClipSave();
    if (rec && Act(p.ui, t, Ui::Id("gm.clip.pause"), Rect(btn.left - kClipBtnW - 8, btn.top, kClipBtnW, kBtnH), m.clip->Paused() ? L"Resume" : L"Pause", false, false, true)) m.clip->Pause(!m.clip->Paused()); // paused, nothing is kept
    if (clips && Act(p.ui, t, Ui::Id("gm.clip.open"), btn, L"Its clips", false, false, true)) OpenRecordings(s.name);
    c.Text(s.name, Rect(x, cy - 20, tw, 24), Weight::Semi, 17, t.text);
    std::wstring line = live ? GameLog::Span(s.secs) + L" so far" : L"Played " + GameLog::Span(s.secs) + L"  ·  " + When(s.start);
    if (kept > 0) line += L"  ·  " + std::to_wstring(kept) + (kept == 1 ? L" clip" : L" clips");
    c.Text(line, Rect(x, cy + 6, tw, 18), Weight::Light, 12, t.textDim);
    if (const GameLog::Total* tot = m.games.TotalOf(s.id))
        c.Text(L"Today " + GameLog::Span(tot->today + (live ? s.secs : 0)) + L"  ·  this week " + GameLog::Span(tot->week + (live ? s.secs : 0)), Rect(x, cy + 24, tw, 18), Weight::Light, 11.5f, t.textLow);
    if (live) m.facesDue = true; // its clock counts up: the next second draws too
}

void ListRunning() { // programs with a window of their own right now, once each
    running.clear();
    struct Ask { std::vector<Running>* out; std::unordered_set<std::wstring> seen; } ask{&running, {}};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        Ask& a = *reinterpret_cast<Ask*>(lp);
        if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || GetWindowTextLengthW(w) == 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        std::wstring exe = sys::games::Lower(sys::games::ExeOf(pid));
        if (exe.empty() || exe.find(L"\\windows\\") != std::wstring::npos || !a.seen.insert(exe).second) return TRUE; // windows' own: never a game
        a.out->push_back({exe, sys::games::ProductName(exe)});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ask));
    std::sort(running.begin(), running.end(), [](const Running& x, const Running& y) { return sys::games::Lower(x.name) < sys::games::Lower(y.name); });
}

void InfoDot(Ui& ui, const Theme& t, D2D1_POINT_2F at, const char* id, const wchar_t* const* lines) { // ⓘ: hovered, its two lines say what the setting beside it is for
    Canvas& c = ui.C();
    D2D1_RECT_F r = {at.x - kInfoR, at.y - kInfoR, at.x + kInfoR, at.y + kInfoR};
    bool hover = ui.Button(Ui::Id(id, 1), Inset(r, -4, -4)).hover;
    ui.Name(std::wstring(lines[0]) + L" " + lines[1]);
    float a = ui.Smooth(Ui::Id(id), hover ? 1.0f : 0, w::kPopFade);
    Color ink = t.textDim.Mix(t.text, a);
    c.Stroke(r, kInfoR, ink, 1.2f);
    c.Text(L"i", Rect(r.left, r.top - 0.5f, 2 * kInfoR, 2 * kInfoR), Weight::Semi, 10.5f, ink, Align::Center);
    if (a > tipFade) tipAt = at, tipFade = a, tipLines = lines; // the one fading in, or still fading out, has the tip
}

void InfoTip(Canvas& c, const Theme& t, const D2D1_RECT_F& area) { // drawn last, over both columns
    if (tipFade < 0.01f) return;
    float wide = 0;
    for (int i = 0; i < 2; ++i) wide = (std::max)(wide, c.Measure(tipLines[i], Weight::Regular, 12));
    wide += kTipSlack; // drawn in exactly its measured width, the line is trimmed to "…"
    float w = wide + 2 * kTipPad, h = 2 * kTipLine + 2 * kTipPad - 6;
    float x = (std::clamp)(tipAt.x - 24, area.left, area.right - w), y = tipAt.y + kInfoR + 8;
    D2D1_RECT_F r = Rect(x, y, w, h);
    w::Pop(c, t, r, w::kPopRadius, tipFade);
    for (int i = 0; i < 2; ++i) c.Text(tipLines[i], Rect(x + kTipPad, y + kTipPad - 3 + i * kTipLine, wide, kTipLine), Weight::Regular, 12, (i ? t.textDim : t.text).Alpha(tipFade));
}

float SettingsH() { return 30 + 4 * kLine + 8 + kLine + kSlideH + 3 * kLine + 12 + 8 + 11 * kLine + kNoteH; } // the card's title, four switches, the sound's look, its volume, three switches; then clips: six choices, two switches, three keys, a note

void Settings(PageCtx& p, const D2D1_RECT_F& r) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    D2D1_RECT_F in = Box(c, t, r, L"Settings");
    float resetX = in.right - history::kResetW, histX = resetX - 8 - history::kW, headY = r.top + 4;
    if (Act(ui, t, Ui::Id("gm.reset"), Rect(resetX, headY, history::kResetW, 24), L"Reset", false, false, Snap(m) != Defaults())) Load(m, Defaults()); // undo brings it back
    int go = history::Buttons(ui, t, "gm.hist", histX, headY + 12, hist, true);
    if (go < 0) Load(m, hist.Undo());
    else if (go > 0) Load(m, hist.Redo());
    float y = in.top;
    auto row = [&](const char* id, const wchar_t* label, int& field, const wchar_t* key, bool enabled = true) {
        if (SwitchRow(ui, t, id, Rect(in.left, y, Width(in), kLine), label, field != 0, enabled)) Flip(m, field, key);
        y += kLine;
    };
    bool on = m.clipOn != 0;
    row("gm.detect", L"Record games", m.clipOn, L"ClipOn"); // off: the page goes back to what it's for and asks again
    row("gm.pill", L"A pill when a game starts", m.gamePill, L"GamePill", on);
    row("gm.summary", L"A pill with the time played when it closes", m.gameSummary, L"GameSummary", on);
    row("gm.quiet", L"Do not disturb while playing", m.gameQuiet, L"GameQuiet", on);
    y += 8;
    c.Text(L"Sounds", Rect(in.left, y, 80, kLine), Weight::Regular, 11.5f, t.text);
    InfoDot(ui, t, {in.left + c.Measure(L"Sounds", Weight::Regular, 11.5f) + kInfoR + 8, y + kLine / 2}, "gm.info", kInfoLines);
    const wchar_t* styles[sys::cues::kStyles];
    for (int i = 0; i < sys::cues::kStyles; ++i) styles[i] = sys::cues::StyleName(i);
    float segW = (std::min)(220.0f, Width(in) - 90);
    int pick = Segmented(ui, t, "gm.style", Rect(in.right - segW, y + (kLine - kSegH) / 2, segW, kSegH), styles, sys::cues::kStyles, m.cueStyle, true);
    if (pick >= 0) m.SetPref(m.cueStyle, L"CueStyle", pick), sys::cues::Play(sys::cues::kGameOn, pick, m.cueVol); // heard as it's picked
    y += kLine;
    float v = m.cueVol / (float)sys::cues::kVolMax;
    bool done = false;
    if (Knob(ui, t, Ui::Id("gm.vol"), Rect(in.left, y, Width(in), kSlideH), L"Volume", std::to_wstring(m.cueVol) + L"%", v, true, done, 0.05f)) m.cueVol = (int)std::lround(v * sys::cues::kVolMax);
    if (done) m.SetPref(m.cueVol, L"CueVolume", m.cueVol), sys::cues::Play(sys::cues::kGameOn, m.cueStyle, m.cueVol);
    y += kSlideH;
    auto cue = [&](const char* id, const wchar_t* label, int& field, const wchar_t* key, sys::cues::Cue which) { // a switch, and a button to hear it
        if (SwitchRow(ui, t, id, Rect(in.left, y, Width(in) - kPlay - 6, kLine), label, field != 0)) Flip(m, field, key);
        if (w::IconBtn(ui, t, Ui::Id(id, 7), {in.right - kPlay / 2, y + kLine / 2}, kPlay, glyph::Play, 16)) sys::cues::Play(which, m.cueStyle, m.cueVol);
        ui.Name(std::wstring(L"Hear: ") + label);
        y += kLine;
    };
    cue("gm.cue.on", L"Sound when a game starts", m.cueOn, L"CueGameOn", sys::cues::kGameOn);
    cue("gm.cue.off", L"Sound when it closes", m.cueOff, L"CueGameOff", sys::cues::kGameOff);
    row("gm.cue.always", L"Even when the pill can be seen", m.cueAlways, L"CueAlways");
    y += 8;
    bool rec = on;
    auto choice = [&](const char* id, const wchar_t* label, const wchar_t* const* names, int n, int cur) { // a label left, its choices right
        c.Text(label, Rect(in.left, y, 90, kLine), Weight::Regular, 11.5f, rec ? t.text : t.textLow);
        float w = (std::min)(230.0f, Width(in) - 96);
        int pick = Segmented(ui, t, id, Rect(in.right - w, y + (kLine - kSegH) / 2, w, kSegH), names, n, cur, rec);
        y += kLine;
        return pick;
    };
    const wchar_t* lengths[] = {L"15 s", L"30 s", L"1 min", L"2 min"};
    int at = (int)(std::find(std::begin(Model::kClipSecs), std::end(Model::kClipSecs), m.clipSecs) - std::begin(Model::kClipSecs));
    if (int got = choice("gm.clip.len", L"Length", lengths, 4, at); got >= 0) m.SetPref(m.clipSecs, L"ClipSeconds", Model::kClipSecs[got]);
    const wchar_t* qualities[] = {L"Light", L"Balanced", L"High"};
    if (int got = choice("gm.clip.q", L"Quality", qualities, 3, m.clipQuality); got >= 0) m.SetPref(m.clipQuality, L"ClipQuality", got);
    const wchar_t* afters[] = {L"Off", L"5 s", L"10 s"};
    int afterAt = (int)(std::find(std::begin(Model::kClipAfters), std::end(Model::kClipAfters), m.clipAfter) - std::begin(Model::kClipAfters));
    float afterY = y;
    if (int got = choice("gm.clip.after", L"After the key", afters, 3, afterAt); got >= 0) m.SetPref(m.clipAfter, L"ClipAfter", Model::kClipAfters[got]);
    InfoDot(ui, t, {in.left + c.Measure(L"After the key", Weight::Regular, 11.5f) + kInfoR + 8, afterY + kLine / 2}, "gm.info.after", kAfterLines);
    const wchar_t* rates[] = {L"30 fps", L"60 fps"};
    if (int got = choice("gm.clip.fps", L"Frame rate", rates, 2, m.clipFps == 30 ? 0 : 1); got >= 0) m.SetPref(m.clipFps, L"ClipFps", got ? 60 : 30);
    const wchar_t* sizes[] = {L"Game's", L"1440p", L"1080p", L"720p"};
    int sizeAt = (int)(std::find(std::begin(Model::kClipHeights), std::end(Model::kClipHeights), m.clipHeight) - std::begin(Model::kClipHeights));
    if (int got = choice("gm.clip.size", L"Size", sizes, 4, sizeAt); got >= 0) m.SetPref(m.clipHeight, L"ClipHeight", Model::kClipHeights[got]); // only ever scaled down
    const wchar_t* sounds[] = {L"None", L"The game", L"All"};
    if (int got = choice("gm.clip.snd", L"Sound", sounds, 3, m.clipAudio); got >= 0) m.SetPref(m.clipAudio, L"ClipAudio", got);
    row("gm.clip.mic", L"Your microphone too", m.clipMic, L"ClipMic", rec);
    row("gm.clip.cursor", L"Show the mouse pointer", m.clipCursor, L"ClipCursor", rec);
    auto keyChip = [&](const char* id, const wchar_t* label, int which, int combo) { // click, then press the key; esc keeps it
        c.Text(label, Rect(in.left, y, 140, kLine), Weight::Regular, 11.5f, rec ? t.text : t.textLow);
        if (which >= 2) InfoDot(ui, t, {in.left + c.Measure(label, Weight::Regular, 11.5f) + kInfoR + 8, y + kLine / 2}, which == 2 ? "gm.info.pause" : "gm.info.long", which == 2 ? kPauseLines : kLongLines); // what the key is for
        D2D1_RECT_F chip = Rect(in.right - kChipW, y + (kLine - kSegH) / 2, kChipW, kSegH);
        bool waiting = m.clipKeyWhich == which;
        std::wstring shown = waiting ? L"Press a key" : (combo & 0xFF) ? StagePrefs::Name((UINT)combo) : L"None";
        if (w::Chip(ui, t, Ui::Id(id), chip, shown, waiting) && rec) m.clipKeyWhich = waiting ? 0 : which;
        if (waiting && ui.In().pressed && !Inside(chip, ui.In().mouse)) m.clipKeyWhich = 0; // clicked away: the key stays as it was
        y += kLine;
    };
    keyChip("gm.clip.key", L"Save shortcut", 1, m.clipKey);
    keyChip("gm.clip.pkey", L"Pause shortcut", 2, m.clipKeyPause);
    keyChip("gm.clip.lkey", L"Long clip shortcut", 3, m.clipKeyLong);
    std::wstring note, sub;
    if (m.clipKeyWhich) note = m.clipKeyWhich >= 2 ? L"Press the key, with Ctrl, Alt or Shift if you like. Backspace: none" : L"Press the key, with Ctrl, Alt or Shift if you like. Esc keeps the old one";
    else if (m.clipKeyClash) note = L"Another app holds that shortcut: pick a different one";
    else if (!m.clipWhy.empty()) note = m.clipWhy;
    else if (m.clip) note = L"Only the game's window is recorded. Holding " + std::to_wstring((int)std::lround(m.clip->Bytes() / 1048576.0)) + L" MB" + (m.clip->HardwareEncoder() ? L"" : L", encoded by the processor");
    else note = L"Only the game's window is recorded, never the screen. About " + std::to_wstring((int)std::lround((kMbitAt1080[m.clipQuality] / 8 + kAudioMbPerSec) * m.ClipKeep())) + L" MB of memory while playing";
    sub = L"Saved to Videos\\Astraeus, a folder for each game";
    c.Text(note, Rect(in.left, y + 2, Width(in), 16), Weight::Light, 11, m.clipKeyClash || !m.clipWhy.empty() ? kDanger : t.textDim);
    c.Text(sub, Rect(in.left, y + 18, Width(in), 16), Weight::Light, 11, t.textLow);
}

std::vector<const GameLog::Total*> TopPlayed(Model& m) { // most played of what's still installed: an uninstalled game leaves the page
    std::vector<const GameLog::Total*> top;
    for (const auto& t : m.games.Totals()) {
        if (top.size() >= (size_t)kTopPlayed) break;
        if (m.games.Installed(t.id)) top.push_back(&t);
    }
    return top;
}

float LibraryH(Model& m) {
    if (adding) return kListHead + (float)(running.size() + 1) * kRowH2;
    size_t top = TopPlayed(m).size();
    size_t hidden = m.games.Hidden() ? 1 + (showHidden ? m.games.Hidden() : 0) : 0;
    return (top ? kListHead + top * kRowH2 + kGap : 0) + kListHead + kSortH + (float)(std::max)((size_t)1, m.games.Library().size()) * kRowH2 + (float)hidden * kRowH2;
}

bool RowBase(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const std::wstring& cover, const std::wstring& exe, const std::wstring& name, const std::wstring& sub, float right);

void NameField(Ui& ui, const Theme& t, Model& m, const D2D1_RECT_F& r) { // the game's new name being typed, in place of its name
    Canvas& c = ui.C();
    c.Fill(r, 7, t.track.Mix(t.cardHi, 0.5f));
    c.Stroke(r, 7, t.accent.Alpha(0.8f), 1);
    bool caret = (int)(ui.Now() * 2) % 2 == 0;
    c.Text(m.fieldEdit + (caret ? L"|" : L""), {r.left + 6, r.top, r.right - 6, r.bottom}, Weight::Medium, 13, t.text);
    ui.KeepAlive();
}

bool HoldRemove(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r) { // fills red while held, fires when full
    Canvas& c = ui.C();
    float prog = 0;
    bool fire = ui.Hold(id, r, kHoldSec, prog);
    bool hover = Inside(r, ui.In().mouse);
    float rad = Height(r) / 2;
    c.Fill(r, rad, kDanger.Alpha(hover ? 0.16f : 0.0f));
    c.Stroke(r, rad, (hover ? kDanger : t.hairTop).Alpha(hover ? 0.7f : 1.0f), 1);
    if (prog > 0) c.Fill(Rect(r.left, r.top, Width(r) * prog, Height(r)), rad, kDanger.Alpha(0.30f));
    c.Text(L"Hold to remove", r, Weight::Medium, 11.5f, hover ? kDanger : t.textDim, Align::Center);
    ui.Name(L"Hold to remove for good");
    return fire;
}

void HiddenRows(PageCtx& p, float& y, const D2D1_RECT_F& col) { // hidden as not games: held, one is forgotten entirely
    Ui& ui = p.ui;
    Model& m = p.m;
    if (hiddenList.size() != m.games.Hidden()) hiddenList = m.games.HiddenGames();
    for (size_t i = 0; i < hiddenList.size(); ++i, y += kRowH2) {
        const sys::games::Game& g = hiddenList[i];
        D2D1_RECT_F row = Rect(col.left, y, Width(col), kRowH2 - 4);
        if (row.bottom < p.area.top || row.top > p.area.bottom) continue;
        RowBase(ui, p.shell, Ui::Id("gm.hid", (int)i), row, g.cover, g.exe, g.name, L"Hidden as not a game", kHoldW + 12);
        if (HoldRemove(ui, p.shell, Ui::Id("gm.hid.rm", (int)i), Rect(row.right - kHoldW - 6, (row.top + row.bottom) / 2 - kBtnH / 2, kHoldW, kBtnH))) {
            m.games.Remove(g.id); // the list changes under the loop: the next frame draws the new one
            hiddenList.clear();
            return;
        }
    }
}

bool RowBase(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const std::wstring& cover, const std::wstring& exe, const std::wstring& name, const std::wstring& sub, float right) { // picture, name, a line under it; true when hovered
    Canvas& c = ui.C();
    bool hover = Inside(r, ui.In().mouse);
    c.Fill(r, 12, t.text.Alpha(0.05f * ui.Smooth(id, hover ? 1.0f : 0, w::kRowFade)));
    float cy = (r.top + r.bottom) / 2;
    Art(c, t, Rect(r.left + 8, cy - kArtRow / 2, kArtRow, kArtRow), kArtRowR, cover, exe);
    float x = r.left + 8 + kArtRow + 12, tw = r.right - right - x;
    c.Text(name, Rect(x, cy - 17, tw, 18), Weight::Medium, 13, t.text);
    c.Text(sub, Rect(x, cy + 1, tw, 16), Weight::Light, 11.5f, t.textDim);
    return hover;
}

void Adding(PageCtx& p, float& y, const D2D1_RECT_F& col) {
    Ui& ui = p.ui;
    const Theme& t = p.shell;
    Model& m = p.m;
    Head(ui, t, Rect(col.left, y, Width(col), kListHead), L"OPEN RIGHT NOW", false);
    if (Act(ui, t, Ui::Id("gm.add.cancel"), Rect(col.right - kBtnW, y + 3, kBtnW, kBtnH), L"Cancel", false, false, true)) adding = false;
    if (Act(ui, t, Ui::Id("gm.add.browse"), Rect(col.right - 2 * kBtnW - 8, y + 3, kBtnW, kBtnH), L"Browse…", true, false, true)) adding = false, PostMessageW(m.notify, WM_APP_GAMEPICK, 0, 0);
    y += kListHead;
    for (size_t i = 0; i < running.size(); ++i, y += kRowH2) {
        const Running& r = running[i];
        D2D1_RECT_F row = Rect(col.left, y, Width(col), kRowH2 - 4);
        if (row.bottom < p.area.top || row.top > p.area.bottom) continue;
        RowBase(ui, t, Ui::Id("gm.run", (int)i), row, L"", r.exe, r.name, r.exe.substr(r.exe.find_last_of(L'\\') + 1), kBtnW + 12);
        if (Act(ui, t, Ui::Id("gm.run.add", (int)i), Rect(row.right - kBtnW - 6, (row.top + row.bottom) / 2 - kBtnH / 2, kBtnW, kBtnH), L"It's a game", false, false, true)) {
            m.games.Mark(r.exe);
            adding = false;
            return;
        }
    }
    if (running.empty()) ui.C().Text(L"Nothing has a window open. Start the game, or browse for it", Rect(col.left + 8, y, Width(col) - 16, kRowH2), Weight::Light, 12, t.textDim);
}

void Library(PageCtx& p, float& y, const D2D1_RECT_F& col) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    auto visible = [&](const D2D1_RECT_F& r) { return r.bottom >= p.area.top && r.top <= p.area.bottom; };
    std::vector<const GameLog::Total*> top = TopPlayed(m);
    if (!top.empty()) {
        Head(ui, t, Rect(col.left, y, Width(col), kListHead), L"MOST PLAYED", false);
        y += kListHead;
        for (size_t i = 0; i < top.size(); ++i, y += kRowH2) {
            const GameLog::Total& g = *top[i];
            D2D1_RECT_F row = Rect(col.left, y, Width(col), kRowH2 - 4);
            if (!visible(row)) continue;
            const sys::games::Game* own = m.games.GameOf(g.id);
            std::wstring cover = own ? own->cover : L"";
            RowBase(ui, t, Ui::Id("gm.top", (int)i), row, cover, g.exe, g.name, L"Today " + GameLog::Span(g.today) + L"  ·  this week " + GameLog::Span(g.week), 120);
            c.Text(GameLog::Span(g.all), Rect(row.right - 120, row.top, 108, Height(row)), Weight::Medium, 12.5f, t.text, Align::Right);
        }
        y += kGap;
    }
    const auto& lib = m.games.Library();
    std::wstring head = L"ON THIS PC  ·  " + std::to_wstring(lib.size());
    Head(ui, t, Rect(col.left, y, Width(col), kListHead), head.c_str(), m.games.Scanning());
    if (Act(ui, t, Ui::Id("gm.add"), Rect(col.right - kBtnW, y + 3, kBtnW, kBtnH), L"Add a game", true, false, true)) adding = true, ListRunning();
    if (Act(ui, t, Ui::Id("gm.rescan"), Rect(col.right - 2 * kBtnW - 8, y + 3, kBtnW, kBtnH), L"Look again", false, m.games.Scanning(), true)) m.games.Rescan();
    y += kListHead;
    const wchar_t* sorts[] = {L"A to Z", L"Last played", L"Most played"};
    float sx = col.right - 4;
    for (int i = 2; i >= 0; --i) { // plain words, right to left: the one in use reads as text, the rest stay quiet
        float w = c.Measure(sorts[i], Weight::Medium, kSortText) + kTipSlack;
        D2D1_RECT_F r = Rect(sx - w, y, w, kSortH);
        Ui::Hit hit = ui.Button(Ui::Id("gm.sort", i), r);
        bool on = m.gameSort == i;
        if (hit.clicked && !on) m.SetPref(m.gameSort, L"GameSort", i);
        ui.Name(std::wstring(L"Sort by ") + sorts[i]);
        c.Text(sorts[i], r, on ? Weight::Medium : Weight::Regular, kSortText, on ? t.text : hit.hover ? t.textDim : t.textLow, Align::Right);
        sx -= w + (i ? kSortGap : 0);
        if (i) c.Text(L"·", Rect(sx, y, kSortGap, kSortH), Weight::Regular, kSortText, t.textLow, Align::Center);
    }
    y += kSortH;
    if (lib.empty()) {
        c.Text(m.games.Scanning() ? L"Looking through your launchers…" : L"No launcher games found. Add one by hand", Rect(col.left + 8, y, Width(col) - 16, kRowH2), Weight::Light, 12, t.textDim);
        y += kRowH2;
    }
    const std::vector<int>& order = m.games.Order(m.gameSort);
    bool menuOpen = !menuFor.empty();
    for (size_t k = 0; k < order.size(); ++k, y += kRowH2) {
        size_t i = (size_t)order[k];
        const sys::games::Game& g = lib[i];
        D2D1_RECT_F row = Rect(col.left, y, Width(col), kRowH2 - 4);
        if (!visible(row)) continue;
        const GameLog::Total* tot = m.games.TotalOf(g.id);
        std::wstring sub = sys::games::SourceName(g.source);
        if (tot) sub += m.gameSort == GameLog::kByRecent ? L"  ·  " + When(tot->last) : L"  ·  " + GameLog::Span(tot->all) + L" played";
        else if (m.gameSort != GameLog::kByName) sub += L"  ·  never played";
        if (!menuOpen && ui.RClick(row)) menuFor = g.id, menuAt = ui.In().mouse, menuSince = ui.Now();
        bool naming = m.field == Model::kFieldGameName && m.gameNaming == g.id;
        bool hover = RowBase(ui, t, Ui::Id("gm.lib", (int)i), row, g.cover, g.exe.empty() && tot ? tot->exe : g.exe, naming ? L"" : g.name, naming ? L"Enter keeps it, its cover is looked for  ·  Esc cancels" : sub, kRowBtns);
        if (naming) NameField(ui, t, m, Rect(row.left + 8 + kArtRow + 6, (row.top + row.bottom) / 2 - 21, Width(row) - kArtRow - 20 - kRowBtns, 22));
        if (!naming && !menuOpen && (hover || ui.Hovered() == Ui::Id("gm.lib.x", (int)i) || ui.Hovered() == Ui::Id("gm.lib.name", (int)i))) {
            if (w::IconBtn(ui, t, Ui::Id("gm.lib.name", (int)i), {row.right - 56, (row.top + row.bottom) / 2}, 28, glyph::Edit, 15)) m.field = Model::kFieldGameName, m.gameNaming = g.id, m.fieldEdit = g.name;
            ui.Name(L"Rename: " + g.name);
            if (w::IconBtn(ui, t, Ui::Id("gm.lib.x", (int)i), {row.right - 22, (row.top + row.bottom) / 2}, 28, glyph::Close, 16)) {
                m.games.Forget(g); // the list changes under the loop: stop here, the next frame draws the new one
                return;
            }
            ui.Name(L"Not a game: " + g.name);
        }
    }
    if (m.games.Hidden()) {
        std::wstring n = std::to_wstring(m.games.Hidden()) + (m.games.Hidden() == 1 ? L" hidden as not a game" : L" hidden as not games");
        c.Text(n, Rect(col.left + 8, y, Width(col) - kBtnW - 24, kRowH2), Weight::Light, 12, t.textDim);
        if (Act(ui, t, Ui::Id("gm.unhide"), Rect(col.right - kBtnW, y + (kRowH2 - kBtnH) / 2, kBtnW, kBtnH), showHidden ? L"Hide list" : L"Show them", false, false, true)) showHidden = !showHidden, hiddenList.clear();
        y += kRowH2;
        if (showHidden) HiddenRows(p, y, col);
    } else {
        showHidden = false;
    }
}

void GameMenu(PageCtx& p) { // right-click on a game: drawn last, over everything
    if (menuFor.empty()) return;
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const sys::games::Game* g = m.games.GameOf(menuFor);
    if (!g || adding) {
        menuFor.clear();
        return;
    }
    enum { kClips, kCover, kRename, kRecord, kNotGame, kItems };
    bool off = m.clipsOff.count(g->id) != 0;
    const wchar_t* labels[] = {L"Recordings", L"Get its cover again", L"Rename", off ? L"Record this game" : L"Don't record this game", L"Not a game"};
    float h = 2 * kMenuPad + (float)kItems * kMenuRow;
    float x = (std::clamp)(menuAt.x, p.area.left, p.area.right - kMenuW), y = menuAt.y;
    if (y + h > p.area.bottom) y = (std::max)(p.area.top, menuAt.y - h); // no room below: it opens upward
    float k = ui.SmoothFrom(Ui::Id("gm.menu.in"), 1, 0, w::kPopFade);
    D2D1_RECT_F box = Rect(x, y + (1 - k) * 6, kMenuW, h);
    w::Pop(c, t, box, w::kPopRadius, k);
    for (int i = 0; i < kItems; ++i) {
        D2D1_RECT_F row = Rect(box.left + kMenuPad, box.top + kMenuPad + i * kMenuRow, kMenuW - 2 * kMenuPad, kMenuRow);
        uint32_t id = Ui::Id("gm.menu.row", i);
        Ui::Hit hit = ui.Button(id, row);
        float hv = w::RowHover(ui, id, hit.hover);
        bool danger = i == kNotGame;
        if (hv > 0.01f) c.Fill(row, w::kRowRadius, (danger ? kDanger.Alpha(0.22f) : t.track).Alpha(hv));
        c.Text(labels[i], Inset(row, 10, 0), Weight::Regular, 12, danger ? kDanger : t.text);
        if (!hit.clicked) continue;
        sys::games::Game pick = *g; // the actions below change the list it lives in
        menuFor.clear();
        if (i == kClips) OpenRecordings(pick.name);
        else if (i == kRecord) m.ClipOff(pick.id, !off);
        else if (i == kCover) m.games.Refetch(pick);
        else if (i == kRename) m.field = Model::kFieldGameName, m.gameNaming = pick.id, m.fieldEdit = pick.name;
        else m.games.Forget(pick);
        return;
    }
    bool away = (ui.In().pressed || ui.RClick(p.area)) && !Inside(box, ui.In().mouse);
    if (away && ui.Now() != menuSince) menuFor.clear(); // a click anywhere else closes it
}

void Column(PageCtx& p, const D2D1_RECT_F& view, float& scroll, const char* id, float content, bool now, bool list, bool side) {
    Canvas& c = p.ui.C();
    float shown = Scroll(p.ui, id, scroll, view, content, !OverSlider(p.ui));
    c.PushClip(view);
    float y = view.top - shown;
    if (now) NowCard(p, Rect(view.left, y, Width(view), kNowH)), y += kNowH + kGap;
    if (list && !adding) {
        float before = y;
        ClipsRow(p, y, view);
        if (y > before) y += kGap;
    }
    if (side) Settings(p, Rect(view.left, y, Width(view), SettingsH())), y += SettingsH() + kGap;
    if (list) {
        if (adding) Adding(p, y, view);
        else Library(p, y, view);
    }
    c.PopClip();
    Fades(c, p.shell, view, shown, content);
}

}

namespace {
std::map<std::wstring, ComPtr<ID2D1Bitmap>> held; // tried ones too, as null: a missing picture isn't looked for every frame
}

void ForgetGameArt() { held.clear(), ForgetClipArt(); } // the device is going: its bitmaps go first, or they'd keep it alive and draw blank on the next one

ID2D1Bitmap* GameArt(Canvas& c, const std::wstring& cover, const std::wstring& exe) {
    if (cover.empty() && exe.empty()) return nullptr;
    std::wstring key = cover + L"|" + exe;
    auto it = held.find(key);
    if (it != held.end()) return it->second.Get();
    if (held.size() >= kArtCap) held.clear();
    ComPtr<ID2D1Bitmap> bmp;
    if (!cover.empty())
        if (ComPtr<IWICBitmapSource> src = gfx::DecodeFile(cover.c_str(), kCoverPx)) c.Target()->CreateBitmapFromWicBitmap(src.Get(), nullptr, &bmp);
    if (!bmp && !exe.empty())
        if (auto px = IconPixels(exe)) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            c.Target()->CreateBitmap({(UINT32)kIconPx, (UINT32)kIconPx}, px->data(), kIconPx * 4, bp, &bmp);
        }
    held[key] = bmp;
    return bmp.Get();
}

void Notice(PageCtx& p) { // recording off: what the page is for, what it costs, and a choice
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    float w = (std::min)(kNoticeW, Width(a) - 24), h = kNoticeH;
    D2D1_RECT_F card = Rect((a.left + a.right - w) / 2, (std::max)(a.top, (a.top + a.bottom - h) / 2), w, h);
    c.Fill(card, 18, CardFill(t));
    D2D1_POINT_2F mark = {(card.left + card.right) / 2, card.top + 44};
    c.Stroke({mark.x - kMarkR, mark.y - kMarkR, mark.x + kMarkR, mark.y + kMarkR}, kMarkR, t.accent, 2);
    c.Text(L"!", Rect(mark.x - kMarkR, mark.y - kMarkR - 1, 2 * kMarkR, 2 * kMarkR), Weight::Semi, 20, t.accent, Align::Center);
    float y = mark.y + kMarkR + 16;
    c.Text(L"Record your games", Rect(card.left, y, w, 24), Weight::Semi, 17, t.text, Align::Center);
    y += 34;
    int mb = (int)std::lround((kMbitAt1080[m.clipQuality] / 8 + kAudioMbPerSec) * m.clipSecs);
    const std::wstring lines[] = {L"This page notices the games you play, and keeps the last " + std::to_wstring(m.clipSecs) + L" seconds",
                                  L"of each in memory, so a shortcut can save it as a clip.",
                                  L"While a game runs that's about " + std::to_wstring(mb) + L" MB of memory and a little of the graphics card.",
                                  L"With no game open, nothing runs. Only the game's window is ever recorded,",
                                  L"never the rest of your screen, and nothing leaves this PC."};
    for (const auto& line : lines) {
        c.Text(line, Rect(card.left + 16, y, w - 32, 18), Weight::Light, 12, t.textDim, Align::Center);
        y += 19;
    }
    y += 18;
    float bw = kNoticeBtnW, gap = 10, x = (card.left + card.right - 2 * bw - gap) / 2;
    if (Act(ui, t, Ui::Id("gm.notice.yes"), Rect(x, y, bw, kBtnH), L"Use it", true, false, true)) m.SetPref(m.clipOn, L"ClipOn", 1);
    if (Act(ui, t, Ui::Id("gm.notice.no"), Rect(x + bw + gap, y, bw, kBtnH), L"No, hide this page", false, false, true)) {
        std::vector<int> pages = m.pages;
        pages.erase(std::remove(pages.begin(), pages.end(), (int)Model::kPgGames), pages.end());
        if (!pages.empty()) m.SetPages(pages); // the rail editor brings it back
    }
}

void DrawGames(PageCtx& p) {
    Model& m = p.m;
    const D2D1_RECT_F& a = p.area;
    if (!m.clipOn) {
        ClipsClose();
        Notice(p);
        return;
    }
    if (m.games.ArtEpoch() != artSeen) artSeen = m.games.ArtEpoch(), ForgetGameArt(); // a cover came in or was dropped: pictures made from the old files go
    if (m.pageVisit != visitSeen) { // a fresh visit: the list read again, and undo starts over
        visitSeen = m.pageVisit;
        adding = false, showHidden = false;
        menuFor.clear();
        ClipsClose();
        m.games.Rescan();
    }
    hist.Sync(m.pageVisit, Snap(m), !p.ui.In().down);
    m.ClipSync(); // a switch flipped here starts or stops recording now, not at the next second
    if (ClipViews(p)) return; // a game's clips or the editor has the page
    bool two = Width(a) >= kTwoCols && Width(a) > Height(a);
    if (two) {
        D2D1_RECT_F main = {a.left, a.top, a.right - kSideW - kGap, a.bottom}, side = {a.right - kSideW, a.top, a.right, a.bottom};
        float clipsH = ClipsRowH(m, Width(main));
        Column(p, main, scrollMain, "gm.scroll", kNowH + kGap + (clipsH > 0 ? clipsH + kGap : 0) + LibraryH(m), true, true, false);
        Column(p, side, scrollSide, "gm.scroll.side", SettingsH(), false, false, true);
    } else {
        float clipsH = ClipsRowH(m, Width(a));
        Column(p, a, scrollMain, "gm.scroll", kNowH + kGap + SettingsH() + kGap + (clipsH > 0 ? clipsH + kGap : 0) + LibraryH(m), true, true, true);
    }
    InfoTip(p.ui.C(), p.shell, a);
    GameMenu(p);
    tipFade = 0; // the dot sets it again next frame while it's on screen
}
