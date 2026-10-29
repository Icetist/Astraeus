// The drawer's live parts: power pills, the activity bubble, paging by wheel, the volume hud and a reloaded config.
#include "drawer.h"

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
    if (hidden || stage || dragging) return; // never over a drag in progress
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

void Drawer::Reconfig() {
    W = cfg.panW + 2 * cfg.fillet; // the ini's sizes, as if it had just started
    H = cfg.panH;
    shapeC.Reset();
    shapeH = shapeW = -1;
    FreeDev();
    Place();
    BuildStrips();
    Redraw();
    Log(L"config reloaded");
}
