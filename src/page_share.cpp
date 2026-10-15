#include <cmath>
#include <cwchar>

#include "anim.h"
#include "glyphs.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

constexpr float kRowOpen = 118; // row height with the delay and volume sliders showing
constexpr int kDelayRangeMs = 500; // slider range, bluetooth lag rarely exceeds this
float scrollTo = 0; // wheel target, eased into "share.scroll"

const wchar_t* Status(const sys::ShareOut& o) {
    if (o.main) return L"Main output";
    if (!o.on) return o.bt ? L"Bluetooth  ·  off" : L"Off";
    if (o.failed) return L"Couldn't play here";
    return o.live ? L"Playing" : L"Waiting for sound";
}

bool Row(PageCtx& p, const D2D1_RECT_F& r, const D2D1_RECT_F& view, sys::ShareOut& o) { // true = list needs a refresh, caller does it after the loop
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    bool live = Inside(view, ui.In().mouse); // parts scrolled under the edges stay inert
    bool hov = live && Inside(r, ui.In().mouse);
    uint32_t id = Ui::Id("share.row", Key(o.id));
    float hover = ui.Smooth(id, hov ? 1.0f : 0, 16);
    float lit = ui.Smooth(id + 1, o.main || (o.on && o.live) ? 1.0f : 0, 10);
    D2D1_RECT_F head = {r.left, r.top, r.right, r.top + kRowH};
    float cy = (head.top + head.bottom) / 2;
    bool refresh = false;

    c.Fill(r, 16, t.cardHi.Alpha(0.35f + 0.65f * hover));
    if (lit > 0.01f) c.FillH(r, 16, t.accent.Alpha(0.16f * lit), t.accent.Alpha(0.02f * lit));
    c.StrokeV(r, 16, t.hairTop.Mix(t.accentHi.Alpha(0.5f), lit), t.hairBot, 1);
    D2D1_POINT_2F ic = {r.left + 28, cy};
    c.Dot(ic, 17, t.track.Mix(t.accent.Alpha(0.28f), lit));
    c.Icon(o.bt || o.phones ? glyph::Headphones : glyph::Speaker, ic, 19, t.textDim.Mix(t.accentHi, lit), o.main || o.on);

    float textR = r.right - 12;
    if (!o.main) {
        D2D1_POINT_2F sw = {r.right - 12 - 23, cy}; // switch is 46 wide
        if (w::Switch(ui, t, id + 2, sw, o.on, live)) {
            o.on = !o.on; // show it now, the engine reports back
            m.share.Enable(o.id, o.on);
            refresh = true;
        }
        D2D1_RECT_F mk = {sw.x - 23 - 10 - 86, cy - 14, sw.x - 23 - 10, cy + 14};
        if (Act(ui, t, id + 4, mk, L"Make main", false, false, live)) { // the source switches, sharing follows it
            sys::SetOut(o.id);
            refresh = true;
        }
        textR = mk.left - 8;
    }
    float tx = r.left + 56;
    c.Text(o.name, {tx, cy - 19, textR, cy + 1}, Weight::Medium, 13.5f, t.text);
    Color sc = o.failed && o.on ? kDanger : o.main || (o.on && o.live) ? t.accent : t.textLow;
    c.Text(Status(o), {tx, cy + 1, textR, cy + 19}, Weight::Light, 11.5f, sc);

    if (!o.on || o.main) return refresh;
    float v = o.delayMs / (float)kDelayRangeMs, vol = o.volume;
    bool done = false;
    D2D1_RECT_F k1 = {tx, head.bottom + 2, r.right - 16, head.bottom + 28};
    if (Knob(ui, t, id + 10, k1, L"Delay", L"+" + std::to_wstring(o.delayMs) + L" ms", v, live, done))
        o.delayMs = (int)std::lround(v * kDelayRangeMs / 5) * 5; // 5 ms steps, preview only
    if (done) m.share.SetDelay(o.id, o.delayMs); // applied on release: each change restarts that device's stream
    D2D1_RECT_F k2 = {tx, k1.bottom + 4, r.right - 16, k1.bottom + 30};
    if (Knob(ui, t, id + 20, k2, L"Volume", std::to_wstring((int)std::lround(vol * 100)) + L"%", vol, live, done)) {
        o.volume = vol;
        m.share.SetVolume(o.id, vol, false); // live and free: the audio engine scales it
    }
    if (done) m.share.SetVolume(o.id, o.volume); // remember it
    return refresh;
}

}

void DrawShare(PageCtx& p, const D2D1_RECT_F& view) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    if (m.shareOuts.empty()) {
        Empty(c, t, view, glyph::Speaker, L"No outputs", L"Connect headphones or speakers to share audio with them");
        return;
    }
    auto height = [](const sys::ShareOut& o) { return (o.on && !o.main ? kRowOpen : kRowH) + kRowGap; };
    float total = kNoteH + 12; // the tip under the list
    for (const auto& o : m.shareOuts) total += height(o);
    float scroll = Scroll(ui, "share.scroll", scrollTo, view, total);
    c.PushClip(view);
    float y = view.top - scroll;
    bool refresh = false;
    for (auto& o : m.shareOuts) {
        float h = height(o);
        if (y + h > view.top && y < view.bottom) refresh = Row(p, {view.left, y, view.right, y + h - kRowGap}, view, o) || refresh;
        y += h;
    }
    c.Text(L"Tip: make your slowest device (usually Bluetooth) the main output, then add delay to the others until they line up.",
           {view.left + 6, y + 4, view.right - 6, y + kNoteH + 8}, Weight::Light, 11.5f, t.textDim);
    c.PopClip();
    Fades(c, t, view, scroll, total);
    if (refresh) m.OnShare(); // after the loop: re-listing replaces the rows it walked
}
