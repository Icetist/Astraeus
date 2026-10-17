// Volume mixer sheet: a small mixing console. One channel strip per app, the master first, faders side by side.
#include <algorithm>
#include <map>

#include "glyphs.h"
#include "page_sheet.h"

using namespace sheet;

namespace {

float scrollTo = 0;
constexpr float kStripW = 96, kStripGap = 10;

bool Strip(PageCtx& p, const D2D1_RECT_F& r, const D2D1_RECT_F& view, uint32_t id, const std::wstring& name, const sys::MixApp* app, bool master,
           float& level, bool muted, bool& muteTap) { // one channel: icon, name, fader, level, mute; true while the fader moves
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.shell;
    bool live = Inside(view, ui.In().mouse);
    float cx = (r.left + r.right) / 2;
    float hover = ui.Smooth(id + 20, live && Inside(r, ui.In().mouse) ? 1.0f : 0, 16);
    c.Fill(r, 18, (master ? t.accent.Alpha(0.16f) : t.cardHi.Alpha(0.35f)).Mix(t.cardHi, 0.3f * hover));
    c.StrokeV(r, 18, master ? t.accentHi.Alpha(0.45f) : t.hairTop, t.hairBot, 1);

    D2D1_POINT_2F ic = {cx, r.top + 30};
    ID2D1Bitmap* bmp = app ? AppIcon(c, *app) : nullptr;
    if (bmp) {
        c.Target()->DrawBitmap(bmp, Rect(ic.x - 16, ic.y - 16, 32, 32), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        c.Dot(ic, 17, master ? t.accent.Alpha(0.30f) : t.track);
        c.Icon(master ? glyph::VolumeUp : glyph::Speaker, ic, 18, master ? t.accentHi : t.textDim, master);
    }
    c.Text(name, Rect(r.left + 8, r.top + 52, Width(r) - 16, 16), Weight::Medium, 11.5f, t.text, Align::Center);
    if (app && app->playing) c.Dot({r.right - 12, r.top + 12}, 3, t.accentHi); // making sound now

    D2D1_RECT_F track = Rect(cx - 3, r.top + 80, 6, Height(r) - 80 - 74); // the fader
    D2D1_RECT_F grab = {cx - 22, track.top - 8, cx + 22, track.bottom + 8};
    bool moved = live && ui.Slider(id, {grab.left, track.top, grab.right, track.bottom}, level, true);
    if (live && Inside(grab, ui.In().mouse)) { // the wheel over a fader moves it, 5% a notch
        float n = ui.Wheel(grab);
        if (n != 0) level = (std::clamp)(level + n * 0.05f, 0.0f, 1.0f), moved = true;
    }
    float shown = ui.Smooth(id + 21, level, moved ? 40.0f : 14.0f);
    float mute = ui.Smooth(id + 22, muted ? 1.0f : 0, 14);
    c.Fill(track, 3, t.track);
    float y = track.bottom - Height(track) * shown;
    if (track.bottom - y > 6) c.Fill({track.left, y, track.right, track.bottom}, 3, t.accent.Mix(t.textLow, mute));
    D2D1_RECT_F knob = Rect(cx - 13, y - 5, 26, 10);
    c.Fill(knob, 5, t.text);
    c.Text(muted ? std::wstring(L"Muted") : w::Pct(level), Rect(r.left, r.bottom - 66, Width(r), 18), Weight::Semi, 12, muted ? t.textLow : t.text, Align::Center);

    D2D1_POINT_2F mb = {cx, r.bottom - 26}; // mute
    D2D1_RECT_F mr = {mb.x - 15, mb.y - 15, mb.x + 15, mb.y + 15};
    Ui::Hit h = live ? ui.Button(id + 23, mr) : Ui::Hit{};
    float mh = ui.Smooth(id + 24, h.hover ? 1.0f : 0, 16);
    c.Dot(mb, 15, (muted ? t.accent.Alpha(0.30f) : t.track).Mix(t.text.Alpha(0.15f), 0.5f * mh));
    c.Icon(muted ? glyph::VolumeOff : glyph::VolumeUp, mb, 16, muted ? t.accentHi : t.textDim.Mix(t.text, mh), muted);
    muteTap = h.clicked;
    return moved;
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

    float hy = s.top + 30;
    c.Text(L"Mixer", Rect(s.left + 24, s.top + 12, 200, 28), Weight::Semi, 21, t.text);
    std::wstring out = m.mixer.Device(); // laptops never list outputs, so ask the mixer's own device
    for (const auto& d : m.outs)
        if (d.id == m.outId) out = d.name;
    if (out.empty()) out = L"No output";
    c.Text(out, Rect(s.left + 24, s.top + 40, Width(s) - 140, 16), Weight::Light, 12, t.textDim);
    if (w::IconBtn(ui, t, Ui::Id("mix.close"), {s.right - 30, hy}, 30, glyph::Close, 18)) m.CloseMixer();
    if (w::IconBtn(ui, t, Ui::Id("mix.more"), {s.right - 64, hy}, 30, glyph::OpenNew, 16)) {
        p.wantClose = true; // windows' own mixer, for outputs per app and the rest
        sys::OpenUri(L"ms-settings:apps-volume");
    }

    D2D1_RECT_F view = {s.left + 16, s.top + 66, s.right - 16, s.bottom - 14};
    const auto& apps = m.mixer.Apps();
    int n = 1 + (int)apps.size();
    float total = n * (kStripW + kStripGap) - kStripGap;
    float maxS = (std::max)(0.0f, total - Width(view));
    scrollTo = (std::clamp)(scrollTo, 0.0f, maxS);
    float scroll = ui.Smooth(Ui::Id("mix.scroll"), scrollTo, 18);
    c.PushClip(view);
    float x = view.left - scroll;
    { // the master channel, first and in the accent
        float level = m.vol;
        bool tap = false;
        if (Strip(p, Rect(x, view.top, kStripW, Height(view)), view, Ui::Id("mix.master"), L"Master", nullptr, true, level, m.muted, tap)) m.SetVol(level);
        if (tap) m.ToggleMute();
        x += kStripW + kStripGap;
    }
    for (const auto& a : apps) {
        if (x + kStripW > view.left && x < view.right) {
            float level = a.volume;
            bool tap = false;
            if (Strip(p, Rect(x, view.top, kStripW, Height(view)), view, Ui::Id("mix.app", Key(a.key)), a.name, &a, false, level, a.muted, tap)) {
                m.mixer.SetVolume(a.key, level);
                if (a.muted) m.mixer.SetMuted(a.key, false); // moving a muted fader brings it back
            }
            if (tap) m.mixer.SetMuted(a.key, !a.muted);
        }
        x += kStripW + kStripGap;
    }
    c.PopClip();
    scrollTo = (std::clamp)(scrollTo - ui.Wheel(view) * 60, 0.0f, maxS); // a wheel no fader took scrolls across, from the next frame
    if (scroll > 1) c.FillH({view.left, view.top, view.left + 28, view.bottom}, 0, t.card, t.card.Alpha(0)); // more to the left / right
    if (scroll < maxS - 1) c.FillH({view.right - 28, view.top, view.right, view.bottom}, 0, t.card.Alpha(0), t.card);
    if (apps.empty())
        c.Text(m.mixer.IsOpen() ? L"Apps show up here once they make a sound" : L"No sound device", Rect(view.left + kStripW + 30, (view.top + view.bottom) / 2 - 10, Width(view) - kStripW - 40, 20),
               Weight::Light, 12.5f, t.textDim);
    if (ui.RClick(s)) m.CloseMixer();
    End(p, frame);
}
