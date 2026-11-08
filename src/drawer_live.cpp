// The drawer's live parts: power pills, the activity bubble, paging by wheel, the volume hud and a reloaded config.
#include "drawer.h"

#include <shellapi.h>

#include "board.h"
#include "deskpill.h"
#include "glyphs.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr GUID kAcdc = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}}; // GUID_ACDC_POWER_SOURCE, without initguid
constexpr GUID kBatPct = {0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}}; // GUID_BATTERY_PERCENTAGE_REMAINING
constexpr ULONGLONG kWheelRestMs = 350; // after a page step the wheel rests this long: one swipe, one page
constexpr float kVolStep = 0.02f; // what windows' own volume keys move
constexpr UINT kHudMs = 1600; // the hud pill goes up again after this

std::wstring Left(int secs) { // "about 3 h 12 min left"
    if (secs < 60) return L""; // unknown, or not a whole minute: no "0 min"
    int h = secs / 3600, m = secs / 60 % 60;
    return L"about " + (h ? std::to_wstring(h) + L" h " : L"") + std::to_wstring(m) + L" min left";
}

}

void Drawer::WatchPower() {
    if (!model.portable) return; // laptops only
    acNote = RegisterPowerSettingNotification(hwnd, &kAcdc, DEVICE_NOTIFY_WINDOW_HANDLE);
    batNote = RegisterPowerSettingNotification(hwnd, &kBatPct, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void Drawer::OnPower(const POWERBROADCAST_SETTING& s) { // laptops: plugged in, unplugged, running low
    if ((s.PowerSetting != kAcdc && s.PowerSetting != kBatPct) || s.DataLength < sizeof(DWORD)) return;
    DWORD v = *reinterpret_cast<const DWORD*>(s.Data);
    model.ReadBattery();
    std::wstring pct = model.batPct >= 0 ? std::to_wstring(model.batPct) + L"%" : L"";
    if (s.PowerSetting == kAcdc) {
        int ac = v == 0 ? 1 : 0; // 0 ac, 1 battery, 2 a ups
        bool changed = acNow >= 0 && ac != acNow; // the first notice only says how things are
        acNow = ac;
        if (ac) model.lowShown = false; // the next discharge may warn again
        if (changed && model.powerPills && !model.quiet && !opened) {
            if (ac) Peek(L"Charging", pct.empty() ? L"Plugged in" : pct + L"  \u00B7  plugged in", glyph::BatteryCharging);
            else {
                std::wstring left = Left(sys::ReadBattery(false).secsLeft);
                Peek(L"On battery", left.empty() ? pct : pct + L"  \u00B7  " + left, glyph::Battery);
            }
        }
    } else {
        int p = (int)v;
        if (acNow != 1 && !model.charging && !model.lowShown && p <= model.batLow && !opened) { // once per discharge, even in do not disturb; an open panel: the next percent after it closes
            model.lowShown = true;
            std::wstring left = Left(sys::ReadBattery(false).secsLeft);
            Peek(L"Battery low (" + std::to_wstring(p) + L"%)", left.empty() ? L"Plug in soon" : L"Plug in soon  \u00B7  " + left, glyph::BatteryLow);
        }
    }
    Redraw();
}

bool Drawer::Bubble() const {
    return islandLook && !opened && !peeking && model.activityBubble && model.Activity() != Model::kActNone;
}

D2D1_RECT_F Drawer::BubbleBox() const { // just past the pill's right shoulder (below it on a side edge), hanging from the edge
    float along = winW / 2 + IslandSpan() / 2 + kBubbleGap, d0 = (kIslandH - kBubble) / 2;
    if (edgeNow == 0) return Rect(along, d0, kBubble, kBubble);
    if (edgeNow == 1) return Rect(d0, along, kBubble, kBubble);
    return Rect(winH - d0 - kBubble, along, kBubble, kBubble);
}

std::wstring Drawer::PillSig() const {
    std::wstring sig;
    if (model.pillMusic && model.hasTrack) sig += model.track.title + L"|" + model.track.artist + (model.track.playing ? L"|p" : L"|-");
    SYSTEMTIME st;
    GetLocalTime(&st);
    sig += L"|" + std::to_wstring(st.wDay * 1440 + st.wHour * 60 + st.wMinute); // the clock, by the minute
    if (model.pillAlways) {
        int g = 0;
        sig += L"|" + model.PillWidget(model.pillLeft, g) + L"|" + model.PillWidget(model.pillRight, g);
    }
    if (Bubble()) sig += L"|" + model.ActivityText();
    return sig;
}

void Drawer::WheelPages(float notches) {
    ULONGLONG now = GetTickCount64();
    if (now - wheelAt < kWheelRestMs) return; // the rest of the swipe that already moved a page
    railWheel = (railWheel > 0) == (notches > 0) ? railWheel + notches : notches; // a turn the other way starts over
    if (std::fabs(railWheel) < 1) return; // a trackpad sends slivers: a page per whole notch
    if (opened && (int)model.pages.size() > kRailShown) { // a rail longer than its window: the wheel scrolls it, a click picks
        railScrollTo += railWheel > 0 ? 1.0f : -1.0f; // RailScroll clamps it
        railWheel = 0;
        wheelAt = now;
        Redraw();
        return;
    }
    int to = model.PageAt(model.SlotOf(page) + (railWheel > 0 ? 1 : -1)); // the next on the rail, as it's ordered
    railWheel = 0;
    wheelAt = now;
    if (opened) {
        SetPage(to);
        return;
    }
    if (to != page) prevPage = page, page = to, pageT = 1; // the closed pill: the panel opens straight onto it
    pageSet = true;
    Open();
}

void Drawer::SyncHud() {
    bool want = model.hud != 0;
    if (want == hudNow) return;
    hudNow = want;
    if (want) { // listeners and the key hook exist only while the option is on
        volWatch.Start(hwnd, WM_APP_HUDVOL);
        brightWatch.Start(hwnd, WM_APP_HUDBRIGHT);
        model.spk.Refresh();
        sys::VolumeKeys::Usable(model.spk.Ready());
        volKeys.Start(hwnd, WM_APP_VOLKEY);
    } else {
        volKeys.Stop();
        volWatch.Stop();
        brightWatch.Stop();
    }
}

void Drawer::OnVolumeKey(UINT vk) { // the key windows never saw: the change is made here, the watch shows it
    model.spk.Refresh(); // the default output may have changed since
    sys::VolumeKeys::Usable(model.spk.Ready()); // nothing to turn up: the next keys go to windows
    if (!volWatch.On() || !model.spk.Ready()) volWatch.Start(hwnd, WM_APP_HUDVOL);
    if (!model.spk.Ready()) return;
    if (vk == VK_VOLUME_MUTE) model.spk.SetMuted(!model.spk.Muted());
    else model.spk.SetLevel(model.spk.Level() + (vk == VK_VOLUME_UP ? kVolStep : -kVolStep));
    model.ReadAudio();
    if (model.hud && !opened) // shown from here too: the watch may still be on the output used before a switch, and the key never reached windows
        ShowHud(model.muted || model.vol < 0.005f ? glyph::VolumeOff : model.vol < 0.5f ? glyph::VolumeDown : glyph::VolumeUp, model.muted ? 0.0f : model.vol,
                model.muted ? L"Muted" : L"Volume");
    Redraw(); // an open panel's slider follows
}

void Drawer::RetryHud() { // once a second while the hud is on: speakers that were missing may be back
    if (!hudNow || (volWatch.On() && model.spk.Ready())) return;
    model.spk.Refresh();
    sys::VolumeKeys::Usable(model.spk.Ready()); // the keys are taken again only once there's something to turn up
    if (model.spk.Ready()) volWatch.Start(hwnd, WM_APP_HUDVOL);
}

void Drawer::ShowHud(int icon, float level, const wchar_t* title) {
    if (hidden || overFull || stage || dragging) return; // never over a drag in progress; over a fullscreen app, only the pill
    bool showing = peeking && peekLevel >= 0;
    peekIcon = icon;
    peekTitle = title;
    peekDetail.clear();
    peekLevel = level;
    if (!showing) { // a fresh drop; while one shows, it just follows the level
        if (!EnsureDev()) return;
        island = islandLook = grow = false;
        peeking = true;
        float reach = Side() ? kPeekSide : kPeekH;
        Resize(reach + overshoot, true);
        spring.target = reach;
        spring.damp = model.reduceMotion ? 1.0f : 0.7f;
    }
    SetTimer(hwnd, kPeekTimer, kHudMs, nullptr);
    Redraw();
}

void Drawer::FitSize() { // the panel's size: the ini's, or the compact column's, which is the sidebar's width dropping from the top
    if (layNow == Model::kLayVertical && edgeNow == 0) {
        W = kSideW + 2 * cfg.fillet;
        H = (std::min)(kCompactMaxH, (work.bottom - monitor.top) / scale - 2 * cfg.fillet); // from the screen's top: pinned, its own reserved room is out of the work area
    } else {
        W = cfg.panW + 2 * cfg.fillet;
        H = cfg.panH;
    }
    shapeC.Reset();
    shapeH = shapeW = -1;
}

void Drawer::Reserve(bool on) { // pinned: the shell keeps the panel's room clear, as it does for a taskbar
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = hwnd;
    if (!on) {
        if (barOn) SHAppBarMessage(ABM_REMOVE, &abd);
        barOn = false;
        return;
    }
    if (!barOn) {
        abd.uCallbackMessage = kBarMsg;
        if (!SHAppBarMessage(ABM_NEW, &abd)) return;
        barOn = true;
    }
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon ? mon : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    LONG d = (LONG)((Depth() + 2) * scale + 0.5f); // its depth on screen, a hair more so a window's shadow doesn't touch it
    abd.uEdge = edgeNow == 0 ? ABE_TOP : edgeNow == 1 ? ABE_LEFT : ABE_RIGHT;
    abd.rc = mi.rcMonitor;
    SHAppBarMessage(ABM_QUERYPOS, &abd); // other bars already there move it inward
    if (edgeNow == 0) abd.rc.bottom = abd.rc.top + d;
    else if (edgeNow == 1) abd.rc.right = abd.rc.left + d;
    else abd.rc.left = abd.rc.right - d;
    SHAppBarMessage(ABM_SETPOS, &abd);
}

void Drawer::SyncBoard() { // each screen set to a dashboard gets one, unless it was switched back to the desktop for now
    auto away = [&](const std::wstring& n) { return std::find(model.boardsAway.begin(), model.boardsAway.end(), n) != model.boardsAway.end(); };
    for (auto& b : boards) // its button asked for the desktop: away until the shortcut or settings bring it back
        if (b && b->Leaving()) model.boardsAway.push_back(b->Name()), b.reset();
    std::vector<std::pair<HMONITOR, std::wstring>> want;
    for (const auto& s : model.boards)
        if (!away(s.screen) && !stage)
            if (HMONITOR m = sys::FindScreenId(s.screen)) want.push_back({m, s.screen}); // unplugged: no window there, its layout waits for it
    std::erase_if(boards, [&](const std::unique_ptr<Board>& b) {
        return !b || !b->Ok() || std::none_of(want.begin(), want.end(), [&](const auto& w) { return w.first == b->Screen() && w.second == b->Name(); });
    });
    static int fails = 0; // a device that won't come: tried again now and then, not every second
    for (const auto& w : want) {
        if (std::any_of(boards.begin(), boards.end(), [&](const auto& b) { return b->Screen() == w.first; })) continue;
        if (fails++ % 30) continue;
        auto b = std::make_unique<Board>(model, w.first, w.second);
        if (b->Ok()) boards.push_back(std::move(b)), fails = 0;
    }
    std::vector<Model::BoardTile> shown; // what's on them all, so polling serves exactly that
    int up = 0;
    Theme media = boards.empty() ? Theme{} : BoardMedia(), shell = boards.empty() ? Theme{} : Shell();
    for (auto& b : boards) {
        b->Second(shell, media);
        if (b->Covered()) continue;
        ++up;
        shown.insert(shown.end(), b->Tiles().begin(), b->Tiles().end());
    }
    model.SetBoardLive(up, std::move(shown));
}

bool Drawer::BoardOn(HMONITOR m) const {
    return std::any_of(boards.begin(), boards.end(), [m](const auto& b) { return b->Screen() == m && !b->Covered(); });
}

void Drawer::BoardKey() { // the screen under the mouse: its dashboard steps aside for the desktop, or comes back
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    for (const auto& s : model.boards) {
        if (sys::FindScreenId(s.screen) != m) continue;
        auto& away = model.boardsAway;
        auto it = std::find(away.begin(), away.end(), s.screen);
        if (it == away.end()) away.push_back(s.screen);
        else away.erase(it);
        SyncBoard(); // now, not on the next second
        SyncHide();
        return;
    }
}

void Drawer::SyncDesk() {
    if (!desk || desk->Editing()) return;
    if (opened) longHeld = false; // the panel took over: a new call brings it back
    bool appsLong = model.deskApps && longHeld && !(deskView && !model.LongPill()); // held over an app: back on a desktop that wears the regular pill, it lets go
    desk->Want(deskOn && (pillView || appsLong) && !hidden && !stage && !opened && !peeking && !dragging && (!fullApp || model.pillFull), mon);
}

void Drawer::DeskEdit(int from) {
    if (!desk || desk->Editing() || stage) return;
    desk->Edit(mon, from);
    SyncHide(); // the panel or the island goes, the editor has the screen
}

void Drawer::WidgetStudio() {
    if (!desk || desk->Editing() || stage) return;
    desk->Studio(mon);
    SyncHide(); // the panel goes, the editor has the screen
}

bool Drawer::DeskNeeds() const { return desk && desk->Needs() && !Unseen(); }

HANDLE Drawer::DeskWait() const { return desk ? desk->Wait() : nullptr; }

void Drawer::DeskFrame() {
    if (!desk) return;
    constexpr float kMaxStep = 0.05f;
    bool fading = shellT < 1 || mediaT < 1;
    if (fading && !MainShown()) { // the panel isn't drawing to carry the crossfade: the pill does
        ULONGLONG at = GetTickCount64();
        StepTheme((std::min)((at - deskFadeAt) / 1000.0f, kMaxStep));
        deskFadeAt = at;
    }
    desk->Frame(Shell());
    if (fading) desk->Refresh();
}

bool Drawer::BoardNeeds() const {
    return !Unseen() && std::any_of(boards.begin(), boards.end(), [](const auto& b) { return b->Needs(); });
}

HANDLE Drawer::BoardWait() const { // the first one with a frame due paces the loop; the others draw with it when their own screen is ready
    for (const auto& b : boards)
        if (b->Needs()) return b->Wait();
    return nullptr;
}

void Drawer::BoardFocus() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    Board* pick = nullptr;
    for (auto& b : boards)
        if (!b->Covered() && (!pick || b->Screen() == m)) pick = b.get();
    if (pick) pick->TakeKeys();
}

bool Drawer::KeyNav(WPARAM key, LPARAM lp) {
    if (!model.a11yKeys) return false;
    bool shift = GetKeyState(VK_SHIFT) < 0, first = !(lp & (1 << 30)); // not a held key's repeat
    switch (key) {
    case VK_TAB:
        ui.Tab(shift ? -1 : 1);
        return true;
    case VK_RETURN:
    case VK_SPACE:
        if (!ui.Focused()) return false;
        if (first) ui.FireFocus(), ui.KeyHold(true);
        return true;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN: {
        if (!ui.Focused()) return false; // no control picked yet: the arrows still change pages
        int dir = key == VK_RIGHT || key == VK_UP ? 1 : -1;
        if (ui.SliderFocused()) ui.StepFocus(dir);
        else ui.Tab(key == VK_RIGHT || key == VK_DOWN ? 1 : -1);
        return true;
    }
    case VK_ESCAPE:
        if (ui.Focused()) { // one step back at a time: the control, then an open sheet, then the panel
            ui.Unfocus();
            return true;
        }
        if (model.settingsOpen) return model.CloseSettings(), true;
        if (model.btOpen) return model.CloseBt(), true;
        if (model.wifiOpen) return model.CloseWifi(), true;
        if (model.calOpen) return model.CloseCal(), true;
        if (model.mixerOpen) return model.CloseMixer(), true;
        return false;
    case 'S':
        if (!model.settingsOpen) ui.Unfocus(), model.OpenSettings(); // the keyboard's way in
        return true;
    default:
        return false;
    }
}

Theme Drawer::BoardMedia() const { return mediaNow.glass ? Theme::From(Seed{mediaSeed.hue, mediaSeed.chroma, 0}) : mediaNow; } // glass has nothing to blur behind a whole screen: its dark twin

void Drawer::BoardFrame() {
    Theme media = BoardMedia();
    Theme shell = Shell();
    bool first = true;
    for (auto& b : boards) {
        if (!b->Needs()) continue;
        if (first || WaitForSingleObject(b->Wait(), 0) == WAIT_OBJECT_0) b->Frame(shell, media); // the pacing one was signalled; the rest only when theirs is
        first = false;
    }
}

void Drawer::Reconfig() {
    FitSize(); // the ini's sizes, as if it had just started
    shapeC.Reset();
    shapeH = shapeW = -1;
    FreeDev();
    Place();
    BuildStrips();
    Redraw();
    Log(L"config reloaded");
}
