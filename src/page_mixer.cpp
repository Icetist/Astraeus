// Volume mixer sheet: a small mixing console. The master and the microphone first, then each group with its apps folded under it,
// then the apps on their own. Groups, chat mix, ducking and profiles are shared with halobar.
#include <algorithm>
#include <cmath>
#include <map>

#include "glyphs.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

constexpr Color kNoTint{0, 0, 0, 0}; // no group color: the look's own accent (a plain Color is opaque black)

float scrollTo = 0;
bool opts = false; // the options card instead of the strips
std::map<int, bool> unfolded; // a group shows its apps

constexpr float kStripW = 96, kStripGap = 10, kMemberW = 88;
constexpr float kIconY = 26, kNameY = 46, kChipTop = 66, kChipH = 17, kChipStep = 21; // a strip, top to bottom: icon, name, its chips
constexpr float kTrackGap = 12, kFootH = 40, kTrackW = 8, kKnobR = 8; // the fader under them, the level and mute under it
constexpr float kChatH = 44, kMeterW = 3, kMeterGap = 6;
constexpr float kWheelStep = 0.05f, kScrollStep = 60;

std::wstring Short(const std::wstring& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + L"…"; }

std::wstring OutName(const Model& m, const std::wstring& id) {
    if (id.empty()) return L"Default";
    for (const auto& d : m.outs)
        if (d.id == id) return d.name;
    return L"Other";
}

std::wstring NextOut(const Model& m, const std::wstring& id) { // default, then each output, then default again
    if (m.outs.empty()) return {};
    if (id.empty()) return m.outs.front().id;
    for (size_t k = 0; k + 1 < m.outs.size(); ++k)
        if (m.outs[k].id == id) return m.outs[k + 1].id;
    return {};
}

bool SmallChip(PageCtx& p, uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, Color dot, bool live) { // a tiny pill under a strip's name
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Ui::Hit h = live ? ui.Button(id, r) : Ui::Hit{};
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    c.Fill(r, kChipH / 2, t.track.Mix(t.cardHi, 0.4f + 0.4f * hov));
    float x = r.left + 8;
    if (dot.a > 0) c.Dot({x + 3, (r.top + r.bottom) / 2}, 3.5f, dot), x += 10;
    c.Text(label, {x, r.top, r.right - 6, r.bottom}, Weight::Medium, 10, t.textDim.Mix(t.text, hov), Align::Center);
    return h.clicked;
}

struct StripLook {
    const wchar_t* name;
    int glyph = 0; // when there's no app icon
    const sys::MixApp* app = nullptr;
    Color tint = kNoTint; // a group's color, or the accent for the master
    float peak = 0; // the live meter, 0..1
    bool muted = false, master = false, member = false;
    int chips = 0; // rows of chips under the name: the fader starts below them
};

float ChipY(const D2D1_RECT_F& r, int row) { return r.top + kChipTop + row * kChipStep; }

bool Strip(PageCtx& p, const D2D1_RECT_F& r, const D2D1_RECT_F& view, uint32_t id, const StripLook& s, float& level, bool& muteTap) { // icon, name, meter, fader, level, mute; true while the fader moves
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    bool live = (Inside(view, ui.In().mouse) || ui.IsActive(id) || ui.Keyed()) && p.m.nameFor < 0; // a fader being dragged keeps following past the view's edge; the keyboard reaches them all
    float cx = (r.left + r.right) / 2;
    float hover = ui.Smooth(id + 20, live && Inside(r, ui.In().mouse) ? 1.0f : 0, 16);
    bool tinted = s.tint.a > 0;
    Color base = s.member ? t.cardHi.Alpha(0.22f) : t.cardHi.Alpha(0.35f);
    c.Fill(r, 18, (tinted ? s.tint.Alpha(s.member ? 0.08f : 0.16f).Mix(base, 0.4f) : base).Mix(t.cardHi, 0.3f * hover));
    c.StrokeV(r, 18, tinted && !s.member ? s.tint.Alpha(0.45f) : t.hairTop, t.hairBot, 1);

    D2D1_POINT_2F ic = {cx, r.top + kIconY};
    ID2D1Bitmap* bmp = s.app ? AppIcon(c, *s.app) : nullptr;
    if (bmp) {
        c.Target()->DrawBitmap(bmp, Rect(ic.x - 14, ic.y - 14, 28, 28), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        Color ink = tinted ? s.tint : t.textDim;
        c.Dot(ic, 15, tinted ? s.tint.Alpha(0.24f) : t.track);
        c.Icon(s.glyph ? s.glyph : glyph::Speaker, ic, 16, ink, tinted);
    }
    c.Text(Short(s.name, 14), Rect(r.left + 6, r.top + kNameY, Width(r) - 12, 16), Weight::Medium, 11.5f, t.text, Align::Center);

    float top = ChipY(r, s.chips) + kTrackGap - (s.chips ? kChipStep - kChipH : 0), bottom = r.bottom - kFootH - 6;
    float fx = cx - (kMeterW + kMeterGap) / 2; // the fader and its meter, centred as a pair
    D2D1_RECT_F track = {fx - kTrackW / 2, top, fx + kTrackW / 2, (std::max)(top + 20, bottom)};
    D2D1_RECT_F grab = {fx - 22, track.top - kKnobR, fx + 22, track.bottom + kKnobR};
    bool moved = live && ui.Slider(id, {grab.left, track.top + kKnobR, grab.right, track.bottom - kKnobR}, level, true);
    if (live) ui.Name(s.name, w::Pct(level));
    if (live && Inside(grab, ui.In().mouse)) { // the wheel over a fader moves it, 5% a notch
        float n = ui.Wheel(grab);
        if (n != 0) level = (std::clamp)(level + n * kWheelStep, 0.0f, 1.0f), moved = true;
    }
    float shown = ui.Smooth(id + 21, level, moved ? 40.0f : 14.0f);
    float mute = ui.Smooth(id + 22, s.muted ? 1.0f : 0, 14);
    float knobHot = ui.Smooth(id + 26, live && (Inside(grab, ui.In().mouse) || ui.IsActive(id)) ? 1.0f : 0, 16);
    Color fill = (tinted ? s.tint : t.accent).Mix(t.textLow, mute);
    c.Fill(track, kTrackW / 2, t.track);
    float y = track.bottom - w::ThumbAt(0, Height(track), kKnobR, shown), end = track.bottom - Height(track) * shown; // the knob's edge meets the ends, the fill runs the whole way
    c.Fill({track.left, (std::min)(end, track.bottom - kTrackW), track.right, track.bottom}, kTrackW / 2, fill);
    float lit = ui.Env(id + 25, s.muted ? 0 : s.peak); // the live meter beside the fader
    D2D1_RECT_F meter = {track.right + kMeterGap, track.top, track.right + kMeterGap + kMeterW, track.bottom};
    c.Fill(meter, kMeterW / 2, t.track.Alpha(0.6f));
    if (lit > 0.01f) c.Fill({meter.left, meter.bottom - Height(meter) * lit, meter.right, meter.bottom}, kMeterW / 2, fill.Alpha(0.85f));
    if (knobHot > 0.01f) c.Dot({fx, y}, kKnobR + 5, fill.Alpha(0.18f * knobHot)); // a soft halo while it's in reach
    c.Dot({fx, y}, kKnobR + knobHot, t.text);
    c.Dot({fx, y}, 3, fill);

    float fy = r.bottom - kFootH / 2 - 2; // the level, and mute beside it
    c.Text(s.muted ? std::wstring(L"Muted") : w::Pct(level), Rect(r.left + 8, fy - 9, Width(r) / 2 + 4, 18), Weight::Semi, 12, s.muted ? t.textLow : t.text, Align::Center);
    D2D1_POINT_2F mb = {r.right - 26, fy};
    D2D1_RECT_F mr = {mb.x - 14, mb.y - 14, mb.x + 14, mb.y + 14};
    Ui::Hit h = live ? ui.Button(id + 23, mr) : Ui::Hit{};
    float mh = ui.Smooth(id + 24, h.hover ? 1.0f : 0, 16);
    c.Dot(mb, 13, (s.muted ? fill.Alpha(0.30f) : t.track).Mix(t.text.Alpha(0.15f), 0.5f * mh));
    int off = s.glyph == glyph::Mic ? glyph::MicOff : glyph::VolumeOff, on = s.glyph == glyph::Mic ? glyph::Mic : glyph::VolumeUp;
    c.Icon(s.muted ? off : on, mb, 15, s.muted ? t.accentHi : t.textDim.Mix(t.text, mh), s.muted);
    muteTap = h.clicked;
    return moved;
}

void AppStrip(PageCtx& p, float x, const D2D1_RECT_F& view, const sys::MixApp& a, int group, bool member) { // one app: on its own, or inside its group
    Model& m = p.m;
    sys::Desk& d = m.desk;
    float w = member ? kMemberW : kStripW;
    D2D1_RECT_F r = Rect(x, view.top, w, Height(view));
    uint32_t id = Ui::Id("mix.app", Key(a.key));
    StripLook s{a.name.c_str(), glyph::Speaker, &a};
    s.peak = a.peak, s.muted = a.muted, s.member = member;
    if (group >= 0) s.tint = Color::Rgb(d.groups[group].color);
    bool groupable = a.key != L"system" && !d.groups.empty();
    bool routable = a.key != L"system" && sys::Mixer::CanRoute() && !m.outs.empty() && (group < 0 || d.groups[group].device.empty());
    s.chips = (int)groupable + (int)routable;
    auto b = d.base.find(a.key);
    float level = member && b != d.base.end() ? b->second : a.volume; // in a group the fader is the app's own share
    bool tap = false;
    if (Strip(p, r, view, id, s, level, tap)) {
        if (member) d.base[a.key] = level, m.DeskSave();
        else m.mixer.SetVolume(a.key, level);
        if (a.muted) m.mixer.SetMuted(a.key, false); // moving a muted fader brings it back
    }
    if (tap) m.mixer.SetMuted(a.key, !a.muted);
    bool live = Inside(view, p.ui.In().mouse) && m.nameFor < 0;
    float cw = w - 16;
    std::wstring gname = group >= 0 ? Short(d.groups[group].name, 9) : std::wstring(L"Add to group");
    if (groupable && SmallChip(p, id + 30, Rect(x + 8, ChipY(r, 0), cw, kChipH), gname, group >= 0 ? Color::Rgb(d.groups[group].color) : kNoTint, live)) {
        int next = group + 1 < (int)d.groups.size() ? group + 1 : -1; // each group in turn, then none
        d.Assign(a.key, next);
        if (next >= 0) d.base[a.key] = a.volume;
        m.DeskSave();
    }
    if (routable && SmallChip(p, id + 32, Rect(x + 8, ChipY(r, groupable ? 1 : 0), cw, kChipH), Short(OutName(m, a.route), 10), kNoTint, live))
        m.mixer.Route(a.key, NextOut(m, a.route));
}

float GroupStrip(PageCtx& p, float x, const D2D1_RECT_F& view, int g) { // the group's own fader, its apps folded under it; returns the width used
    Model& m = p.m;
    sys::Desk& d = m.desk;
    sys::DeskGroup& grp = d.groups[g];
    float peak = 0;
    std::vector<const sys::MixApp*> members;
    for (const auto& a : m.mixer.Apps())
        if (d.GroupOf(a.key) == g) members.push_back(&a), peak = (std::max)(peak, a.peak);
    D2D1_RECT_F r = Rect(x, view.top, kStripW, Height(view));
    uint32_t id = Ui::Id("mix.group", g);
    StripLook s{grp.name.c_str(), glyph::kGroupIcons[grp.icon % sys::Desk::kIcons]};
    s.tint = Color::Rgb(grp.color), s.peak = peak, s.muted = grp.muted, s.chips = 2;
    float level = grp.volume;
    bool tap = false;
    if (Strip(p, r, view, id, s, level, tap)) grp.volume = level, d.profile = -1, m.DeskSave();
    if (tap) grp.muted = !grp.muted, d.profile = -1, m.DeskSave();
    bool live = Inside(view, p.ui.In().mouse) && m.nameFor < 0;
    bool open = unfolded[g];
    std::wstring count = std::to_wstring(members.size()) + (members.size() == 1 ? L" app" : L" apps");
    if (SmallChip(p, id + 30, Rect(x + 8, ChipY(r, 0), kStripW - 16, kChipH), count + (open ? L"  ‹" : L"  ›"), kNoTint, live)) unfolded[g] = !open;
    if (SmallChip(p, id + 32, Rect(x + 8, ChipY(r, 1), kStripW - 16, kChipH), L"Edit", kNoTint, live)) m.deskEdit = g, opts = false;
    float used = kStripW + kStripGap;
    if (open)
        for (const auto* a : members) {
            if (x + used + kMemberW > view.left && x + used < view.right) AppStrip(p, x + used, view, *a, g, true);
            used += kMemberW + kStripGap;
        }
    return used;
}

void AddStrip(PageCtx& p, float x, const D2D1_RECT_F& view) { // a new group, while there's room for one
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    D2D1_RECT_F r = Rect(x, view.top, kStripW, Height(view));
    Ui::Hit h = Inside(view, ui.In().mouse) && p.m.nameFor < 0 ? ui.Button(Ui::Id("mix.add"), r) : Ui::Hit{};
    float hov = ui.Smooth(Ui::Id("mix.add", 1), h.hover ? 1.0f : 0, 16);
    c.Stroke(Inset(r, 1, 1), 18, t.hairTop.Mix(t.accent, hov), 1);
    D2D1_POINT_2F mid = {(r.left + r.right) / 2, (r.top + r.bottom) / 2 - 12};
    c.Icon(glyph::Add, mid, 22, t.textDim.Mix(t.accentHi, hov), false);
    c.Text(L"New group", Rect(r.left, mid.y + 18, Width(r), 16), Weight::Medium, 11.5f, t.textDim.Mix(t.text, hov), Align::Center);
    if (h.clicked) {
        int g = p.m.desk.AddGroup();
        if (g >= 0) p.m.DeskSave(), p.m.deskEdit = g;
    }
}

void ChatBar(PageCtx& p, const D2D1_RECT_F& r) { // game on the left, chat on the right: one slider between them
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    sys::Desk& d = p.m.desk;
    const auto& game = d.groups[d.game];
    const auto& chat = d.groups[d.chat];
    c.Fill(r, Height(r) / 2, t.cardHi.Alpha(0.35f));
    c.Icon(glyph::kGroupIcons[game.icon % sys::Desk::kIcons], {r.left + 22, (r.top + r.bottom) / 2}, 17, Color::Rgb(game.color), true);
    c.Text(Short(game.name, 10), Rect(r.left + 38, r.top, 90, Height(r)), Weight::Medium, 11.5f, t.text);
    c.Icon(glyph::kGroupIcons[chat.icon % sys::Desk::kIcons], {r.right - 22, (r.top + r.bottom) / 2}, 17, Color::Rgb(chat.color), true);
    c.Text(Short(chat.name, 10), Rect(r.right - 128, r.top, 90, Height(r)), Weight::Medium, 11.5f, t.text, Align::Right);
    D2D1_RECT_F track = {r.left + 134, (r.top + r.bottom) / 2 - 3, r.right - 134, (r.top + r.bottom) / 2 + 3};
    float v = (1 - d.chatMix) / 2; // left is all game
    uint32_t id = Ui::Id("mix.chat");
    constexpr float kChatKnob = 8;
    bool moved = ui.Slider(id, {track.left + kChatKnob, r.top, track.right - kChatKnob, r.bottom}, v, false);
    if (float n = ui.Wheel(r)) v = (std::clamp)(v - n * kWheelStep, 0.0f, 1.0f), moved = true;
    if (moved) d.chatMix = (std::fabs(v - 0.5f) < 0.03f ? 0.0f : 1 - 2 * v), d.profile = -1, p.m.DeskSave(); // snaps to even near the middle
    float shown = ui.Smooth(id + 1, (1 - d.chatMix) / 2, 18);
    c.Fill(track, 3, t.track);
    float mid = (track.left + track.right) / 2, x = w::ThumbAt(track.left, track.right, kChatKnob, shown);
    c.Fill({(std::min)(mid, x), track.top, (std::max)(mid, x), track.bottom}, 3, Color::Rgb(d.chatMix > 0 ? game.color : chat.color));
    c.Fill(Rect(mid - 1, track.top - 4, 2, 14), 1, t.textLow);
    c.Dot({x, (track.top + track.bottom) / 2}, kChatKnob, t.text);
}

bool Toggle(PageCtx& p, const char* id, const D2D1_RECT_F& r, const wchar_t* label, bool on) { // label left, switch right
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    c.Text(label, {r.left, r.top, r.right - 56, r.bottom}, Weight::Regular, 11.5f, t.text.Mix(t.textDim, 0.25f));
    return w::Switch(p.ui, t, Ui::Id(id), {r.right - 24, (r.top + r.bottom) / 2}, on);
}

void Editor(PageCtx& p, const D2D1_RECT_F& r) { // one group: name, color, icon, output, chat role, ducking
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    sys::Desk& d = m.desk;
    int g = m.deskEdit;
    sys::DeskGroup& grp = d.groups[g];
    float x = r.left + 8, y = r.top + 4, w = Width(r) - 16;
    D2D1_RECT_F field = Rect(x, y, (std::min)(w, 260.0f), 30); // the name: click to type
    bool typing = m.nameFor == g;
    c.Fill(field, 10, t.track.Mix(t.cardHi, typing ? 0.6f : 0.3f));
    c.Stroke(field, 10, typing ? t.accent.Alpha(0.8f) : t.hairTop, 1);
    std::wstring shown = typing ? m.nameEdit + ((int)(ui.Now() * 2) % 2 ? L"" : L"|") : grp.name;
    c.Text(shown, Inset(field, 12, 0), Weight::Medium, 13, t.text);
    if (typing) ui.KeepAlive(); // the caret blinks
    if (!typing && ui.Button(Ui::Id("mix.ed.name"), field).clicked) m.nameFor = g, m.nameEdit = grp.name;
    y += 42;
    for (int k = 0; k < sys::Desk::kColors; ++k) { // color
        D2D1_POINT_2F o = {x + 11 + k * 28.0f, y + 11};
        bool on = (grp.color & 0xFFFFFF) == sys::Desk::Swatch(k);
        if (ui.Button(Ui::Id("mix.ed.col", k), {o.x - 11, o.y - 11, o.x + 11, o.y + 11}).clicked) grp.color = sys::Desk::Swatch(k), m.DeskSave();
        c.Dot(o, on ? 10.0f : 8.0f, Color::Rgb(sys::Desk::Swatch(k)));
        if (on) c.Stroke({o.x - 13, o.y - 13, o.x + 13, o.y + 13}, 13, t.text.Alpha(0.7f), 1.5f);
    }
    float ix = x + sys::Desk::kColors * 28.0f + 20;
    for (int k = 0; k < sys::Desk::kIcons; ++k) { // icon
        D2D1_POINT_2F o = {ix + 13 + k * 30.0f, y + 11};
        bool on = grp.icon == k;
        if (ui.Button(Ui::Id("mix.ed.icon", k), {o.x - 13, o.y - 13, o.x + 13, o.y + 13}).clicked) grp.icon = k, m.DeskSave();
        if (on) c.Dot(o, 13, Color::Rgb(grp.color).Alpha(0.25f));
        c.Icon(glyph::kGroupIcons[k], o, 17, on ? Color::Rgb(grp.color) : t.textDim, on);
    }
    y += 36;
    c.Text(L"Output", Rect(x, y, 80, 24), Weight::Regular, 11.5f, t.textDim);
    float cx = x + 70;
    for (int k = -1; k < (int)m.outs.size() && cx < r.right - 60; ++k) { // default, then each output
        std::wstring id = k < 0 ? L"" : m.outs[k].id, label = k < 0 ? L"Default" : Short(m.outs[k].name, 16);
        float cw = 22 + 7.0f * (float)label.size();
        if (w::Chip(ui, t, Ui::Id("mix.ed.out", k + 1), Rect(cx, y, cw, 24), label, grp.device == id)) grp.device = id, d.profile = -1, m.DeskSave();
        cx += cw + 6;
    }
    y += 34;
    static const wchar_t* const kRoles[] = {L"None", L"Game side", L"Chat side"};
    int role = d.game == g ? 1 : d.chat == g ? 2 : 0;
    c.Text(L"Chat mix", Rect(x, y, 80, 28), Weight::Regular, 11.5f, t.textDim);
    int pick = Segmented(ui, t, "mix.ed.role", Rect(x + 70, y, 270, 28), kRoles, 3, role, true);
    if (pick >= 0 && pick != role) {
        if (d.game == g) d.game = -1;
        if (d.chat == g) d.chat = -1;
        if (pick == 1) d.game = g;
        if (pick == 2) d.chat = g;
        m.DeskSave();
    }
    y += 40;
    float half = (w - 20) / 2;
    if (Toggle(p, "mix.ed.ducks", Rect(x, y, half, 28), L"Lower while others play", grp.ducks)) grp.ducks = !grp.ducks, m.DeskSave();
    if (Toggle(p, "mix.ed.lowers", Rect(x + half + 20, y, half, 28), L"Lowers the others", grp.lowers)) grp.lowers = !grp.lowers, m.DeskSave();
    y += 44;
    if (Act(ui, t, Ui::Id("mix.ed.del"), Rect(x, y, 120, 30), L"Delete group", false, false, true)) {
        for (const auto& a : grp.apps) m.mixer.SetMuted(a, false); // nothing stays muted by a group that's gone
        d.RemoveGroup(g);
        unfolded.clear();
        m.deskEdit = m.nameFor = -1;
        m.DeskSave();
        return;
    }
    if (Act(ui, t, Ui::Id("mix.ed.done"), Rect(r.right - 128, y, 120, 30), L"Done", true, false, true)) {
        if (m.nameFor >= 0) m.CommitName();
        m.deskEdit = -1;
    }
}

void Options(PageCtx& p, const D2D1_RECT_F& r) { // ducking depth, shortcuts, the banner, profiles
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    sys::Desk& d = m.desk;
    float x = r.left + 8, w = Width(r) - 16, half = (w - 24) / 2, y = r.top + 4;
    float v = (d.duckBy - sys::Desk::kDuckMin) / (float)(sys::Desk::kDuckMax - sys::Desk::kDuckMin);
    bool done = false;
    constexpr float kDuckStep = 5.0f / (sys::Desk::kDuckMax - sys::Desk::kDuckMin);
    if (Knob(ui, t, Ui::Id("mix.duck"), Rect(x, y, half, kSlideH), L"Lower by", std::to_wstring(d.duckBy) + L"%", v, true, done, kDuckStep))
        d.duckBy = sys::Desk::kDuckMin + (int)std::lround(v * (sys::Desk::kDuckMax - sys::Desk::kDuckMin) / 5) * 5;
    if (done) m.DeskSave();
    if (Toggle(p, "mix.keys", Rect(x + half + 24, y, half, 24), L"Shortcuts", d.keys)) d.keys = !d.keys, m.DeskSave();
    if (Toggle(p, "mix.osd", Rect(x + half + 24, y + 26, half, 24), L"Show what a shortcut did", d.osd)) d.osd = !d.osd, m.DeskSave();
    y += kSlideH + 12;
    c.Text(L"Ctrl+Alt with  M microphone  ·  P next profile  ·  [ ] chat mix  ·  1-8 mute a group", Rect(x, y, w, 16), Weight::Light, 10.5f, t.textLow);
    y += 28;
    c.Text(L"PROFILES", Rect(x, y, w, 16), Weight::Semi, 10.5f, t.textDim);
    y += 22;
    float cx = x;
    for (int k = 0; k < (int)d.profiles.size(); ++k) { // recall, or x to forget
        const std::wstring& name = d.profiles[k].name;
        float cw = 34 + 7.0f * (float)name.size();
        if (w::Chip(ui, t, Ui::Id("mix.prof", k), Rect(cx, y, cw - 24, 26), name, d.profile == k)) d.Recall(k), m.DeskSave();
        if (w::IconBtn(ui, t, Ui::Id("mix.prof.x", k), {cx + cw - 12, y + 13}, 22, glyph::Close, 13)) {
            d.profiles.erase(d.profiles.begin() + k);
            d.profile = -1;
            m.DeskSave();
            break;
        }
        cx += cw + 8;
    }
    if ((int)d.profiles.size() < sys::Desk::kMaxProfiles && w::Chip(ui, t, Ui::Id("mix.prof.add"), Rect(cx, y, 150, 26), L"Save current mix", false))
        d.SaveProfile((int)d.profiles.size(), L""), m.DeskSave();
    y += 40;
    c.Text(L"A profile keeps each group's level, mute and output, and the chat mix.", Rect(x, y, w, 16), Weight::Light, 10.5f, t.textLow);
}

constexpr float kOutRowH = 34; // the outputs, across the top of the strips

float OutRow(PageCtx& p, const D2D1_RECT_F& body) { // every output as a chip: a tap makes it the default, so every app following the default goes with it; returns the height used
    Model& m = p.m;
    if (m.outs.size() < 2) return 0;
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    constexpr float kH = 26, kGap = 6, kLabelW = 54;
    c.Icon(glyph::Speaker, {body.left + 10, body.top + kH / 2}, 15, t.textDim);
    c.Text(L"Output", Rect(body.left + 22, body.top, kLabelW, kH), Weight::Regular, 11.5f, t.textDim);
    float x = body.left + 22 + kLabelW;
    for (size_t i = 0; i < m.outs.size(); ++i) {
        std::wstring label = Short(m.outs[i].name, 24);
        float cw = c.Measure(label, Weight::Medium, 11) + 26;
        if (x + cw > body.right) break;
        if (w::Chip(ui, t, Ui::Id("mix.outs", (int)i), Rect(x, body.top, cw, kH), label, m.outs[i].id == m.outId)) m.SetOutTo(m.outs[i].id);
        x += cw + kGap;
    }
    return kOutRowH;
}

void Header(PageCtx& p, const D2D1_RECT_F& s) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    float hy = s.top + 30;
    c.Text(L"Mixer", Rect(s.left + 24, s.top + 12, 200, 28), Weight::Semi, 21, t.text);
    if (m.outs.size() < 2) { // one output: nothing to choose, its name says where the sound goes
        std::wstring out = m.mixer.Device(); // laptops never list outputs, so ask the mixer's own device
        for (const auto& d : m.outs)
            if (d.id == m.outId) out = d.name;
        c.Text(out.empty() ? L"No output" : out, Rect(s.left + 24, s.top + 40, 240, 16), Weight::Light, 12, t.textDim);
    }
    if (w::IconBtn(ui, t, Ui::Id("mix.close"), {s.right - 30, hy}, 30, glyph::Close, 18)) m.CloseMixer();
    if (w::IconBtn(ui, t, Ui::Id("mix.opts"), {s.right - 64, hy}, 30, opts ? glyph::Check : glyph::Tune, 17)) opts = !opts, m.deskEdit = -1;
    float cx = s.right - 96;
    const auto& prof = m.desk.profiles;
    for (int k = (int)prof.size() - 1; k >= 0 && cx > s.left + 300; --k) { // the profiles, one tap away
        float cw = 24 + 7.0f * (float)(std::min)(prof[k].name.size(), (size_t)12);
        cx -= cw;
        if (w::Chip(ui, t, Ui::Id("mix.head.prof", k), Rect(cx, hy - 13, cw, 26), Short(prof[k].name, 12), m.desk.profile == k)) m.desk.Recall(k), m.DeskSave();
        cx -= 6;
    }
}

}

ID2D1Bitmap* AppIcon(Canvas& c, const sys::MixApp& a) { // the app's icon on this device, made once per app
    struct Held {
        ComPtr<ID2D1Bitmap> bmp;
        const void* src = nullptr;
    };
    static std::map<std::wstring, Held> held;
    static ID2D1RenderTarget* owner = nullptr;
    if (owner != c.Target()) held.clear(), owner = c.Target(); // a new device can't use the old bitmaps
    if (!a.icon) return nullptr;
    if (held.size() > 64) held.clear(); // never piles up
    Held& h = held[a.key];
    if (!h.bmp || h.src != a.icon.get()) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        h.bmp.Reset();
        c.Target()->CreateBitmap({32, 32}, a.icon->data(), 32 * 4, bp, &h.bmp);
        h.src = a.icon.get();
    }
    return h.bmp.Get();
}

void DrawMixer(PageCtx& p, float k) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    const D2D1_RECT_F& s = p.area;
    Frame frame = Begin(p, k, false);
    Header(p, s);
    if (m.deskEdit >= (int)m.desk.groups.size()) m.deskEdit = -1; // halobar deleted it meanwhile
    D2D1_RECT_F body = {s.left + 16, s.top + 66, s.right - 16, s.bottom - 14};
    if (m.deskEdit >= 0 || opts) {
        if (m.deskEdit >= 0) Editor(p, body);
        else Options(p, body);
        if (ui.RClick(s)) m.deskEdit = -1, opts = false, m.nameFor = -1;
        End(p, frame);
        return;
    }
    bool chat = m.desk.Chatting();
    float outsH = OutRow(p, body);
    D2D1_RECT_F view = {body.left, body.top + outsH, body.right, body.bottom - (chat ? kChatH + 10 : 0)};
    const auto& apps = m.mixer.Apps();
    float total = 2 * (kStripW + kStripGap); // master and mic
    for (int g = 0; g < (int)m.desk.groups.size(); ++g) {
        total += kStripW + kStripGap;
        if (unfolded[g])
            for (const auto& a : apps) total += m.desk.GroupOf(a.key) == g ? kMemberW + kStripGap : 0;
    }
    for (const auto& a : apps) total += m.desk.GroupOf(a.key) < 0 ? kStripW + kStripGap : 0;
    if ((int)m.desk.groups.size() < sys::Desk::kMaxGroups) total += kStripW + kStripGap;
    float maxS = (std::max)(0.0f, total - kStripGap - Width(view));
    scrollTo = (std::clamp)(scrollTo, 0.0f, maxS);
    float scroll = ui.Smooth(Ui::Id("mix.scroll"), scrollTo, 18);
    c.PushClip(view);
    float x = view.left - scroll;
    { // the master channel, first and in the accent
        StripLook s{L"Master", glyph::VolumeUp};
        s.tint = t.accent, s.muted = m.muted, s.master = true;
        float level = m.vol;
        bool tap = false;
        if (Strip(p, Rect(x, view.top, kStripW, Height(view)), view, Ui::Id("mix.master"), s, level, tap)) m.SetVol(level);
        if (tap) m.ToggleMute();
        x += kStripW + kStripGap;
    }
    { // the microphone, its input picked under its name
        std::wstring name = L"Microphone";
        for (const auto& d : m.mics)
            if (d.id == m.micId) name = d.name;
        StripLook s{name.c_str(), glyph::Mic};
        s.muted = m.micMuted, s.chips = m.mics.size() > 1 ? 1 : 0;
        float level = m.micVol;
        bool tap = false;
        uint32_t id = Ui::Id("mix.mic");
        if (Strip(p, Rect(x, view.top, kStripW, Height(view)), view, id, s, level, tap)) m.SetMic(level);
        if (tap) m.ToggleMic();
        if (m.mics.size() > 1 && SmallChip(p, id + 30, Rect(x + 8, ChipY(Rect(x, view.top, kStripW, Height(view)), 0), kStripW - 16, kChipH), L"Switch input", kNoTint, Inside(view, ui.In().mouse))) {
            size_t cur = 0;
            for (size_t i = 0; i < m.mics.size(); ++i)
                if (m.mics[i].id == m.micId) cur = i;
            const auto& next = m.mics[(cur + 1) % m.mics.size()];
            if (sys::SetIn(next.id)) m.micId = next.id, m.mic.Refresh(), m.ReadAudio();
        }
        x += kStripW + kStripGap;
    }
    for (int g = 0; g < (int)m.desk.groups.size(); ++g) x += GroupStrip(p, x, view, g);
    for (const auto& a : apps) {
        if (m.desk.GroupOf(a.key) >= 0) continue;
        if (x + kStripW > view.left && x < view.right) AppStrip(p, x, view, a, -1, false);
        x += kStripW + kStripGap;
    }
    if ((int)m.desk.groups.size() < sys::Desk::kMaxGroups) AddStrip(p, x, view);
    c.PopClip();
    if (!OverSlider(ui)) scrollTo = (std::clamp)(scrollTo - ui.Wheel(view) * kScrollStep, 0.0f, maxS); // a wheel no fader took scrolls across, from the next frame
    if (scroll > 1) c.FillH({view.left, view.top, view.left + 28, view.bottom}, 0, t.card, t.card.Alpha(0)); // more to the left / right
    if (scroll < maxS - 1) c.FillH({view.right - 28, view.top, view.right, view.bottom}, 0, t.card.Alpha(0), t.card);
    if (chat) ChatBar(p, {body.left, body.bottom - kChatH, body.right, body.bottom});
    if (!m.mixer.IsOpen()) c.Text(L"No sound device", Rect(view.left + 2 * (kStripW + kStripGap) + 20, (view.top + view.bottom) / 2 - 10, 300, 20), Weight::Light, 12.5f, t.textDim);
    if (ui.RClick(s)) m.CloseMixer();
    End(p, frame);
}
