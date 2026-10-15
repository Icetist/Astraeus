#include <algorithm>
#include <cmath>

#include "anim.h"
#include "glyphs.h"
#include "pages.h"

namespace {

constexpr float kPi = 3.14159265f;
float preview = -1; // seek preview while dragging, -1 = none

void Wave(PageCtx& p, const D2D1_RECT_F& r, float fraction, bool playing, bool canSeek, bool known) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.media;
    uint32_t id = Ui::Id("md.seek");
    float v = !known ? 1.0f : preview >= 0 ? preview : fraction; // unknown length: whole line waves, no playhead
    if (canSeek && ui.Slider(id, r, v, false)) preview = v; // preview seek while dragging
    if (ui.IsActive(id) && ui.In().released) { // commit seek on release only
        p.m.media.Seek(v);
        preview = -1;
    }
    if (!ui.IsActive(id)) preview = -1;

    float amp = ui.Smooth(id + 1, playing && !ui.IsActive(id) ? 3.2f : 0, 5); // wave flattens when paused or dragging
    float cy = (r.top + r.bottom) / 2;
    float x = r.left + Width(r) * anim::Clamp01(v);
    const float gap = 7; // gap around the head
    float a0 = r.left + 2, a1 = known ? x - gap : r.right - 2; // played part, left of head
    WaveLine(c, a0, a1, cy, amp, ui.Now(), t.accent, 4, 28);
    if (amp >= 0.05f && a1 > a0) ui.KeepAlive(); // keep repainting while wave moves
    if (!known) return;
    if (x + gap < r.right - 2) c.Line({x + gap, cy}, {r.right - 2, cy}, t.track, 4); // unplayed part stays flat
    float grow = ui.Smooth(id + 2, ui.IsActive(id) || Inside(r, ui.In().mouse) ? 1.0f : 0, 16); // head grows on hover/drag
    c.Fill({x - 2, cy - 11 - 3 * grow, x + 2, cy + 11 + 3 * grow}, 2, t.accentHi); // vertical bar playhead
}

struct Ambient {
    double lastBeat = 0;
    float slow = 0, beat = 0, phase = 0;
};
Ambient amb;

void DrawViz(PageCtx& p, const D2D1_RECT_F& r) {
    if (!p.m.cfg.viz || !p.m.hasTrack) return;
    Ui& ui = p.ui;
    bool playing = p.m.track.playing;
    float left = 0, right = 0;
    if (playing) p.m.meter.Read(left, right); // paused: levels decay via envelope
    float l = ui.Env(Ui::Id("viz.l"), std::sqrt(left)); // sqrt lifts quiet levels
    float rr = ui.Env(Ui::Id("viz.r"), std::sqrt(right));
    float level = (l + rr) / 2;

    float dt = ui.Dt();
    double now = ui.Now();
    amb.slow += (level - amb.slow) * (std::min)(1.0f, dt * 2.5f); // slow running avg of level
    if (playing && level > amb.slow * 1.22f + 0.035f && now - amb.lastBeat > 0.24) { // beat = level spike over slow avg, 240ms cooldown
        amb.beat = 1;
        amb.lastBeat = now;
    }
    amb.beat *= std::exp(-dt * 5); // beat pulse decays exponentially
    amb.phase = std::fmod(amb.phase + dt * (0.12f + 0.55f * level), 2 * kPi * 100); // louder = faster drift

    if (playing || l > 0.01f || rr > 0.01f || amb.beat > 0.01f) ui.KeepAlive(); // animate until fully faded
    float glow = (std::max)(level, amb.beat * 0.5f);
    if (glow < 0.005f) return;

    const Theme& t = p.media;
    Canvas& c = ui.C();
    float w = Width(r), h = Height(r), ph = amb.phase, swell = 1 + 0.18f * amb.beat; // beat swells blobs 18%
    D2D1_POINT_2F a = {r.left + w * (0.30f + 0.16f * std::sin(ph * 0.9f)), r.top + h * (0.62f + 0.30f * std::sin(ph * 1.3f + 1.0f))}; // two blobs wander on lissajous paths
    D2D1_POINT_2F b = {r.left + w * (0.72f + 0.16f * std::sin(ph * 0.7f + 2.1f)), r.top + h * (0.38f + 0.30f * std::cos(ph * 1.1f))};
    c.PushClip(r);
    c.Glow(a, w * (0.30f + 0.22f * l) * swell, h * (0.60f + 0.45f * l) * swell, t.accent.Alpha(0.10f + 0.38f * l + 0.12f * amb.beat)); // left channel drives blob a, right drives b
    c.Glow(b, w * (0.26f + 0.20f * rr) * swell, h * (0.55f + 0.40f * rr) * swell,
           t.accentHi.Alpha(0.06f + 0.30f * rr + 0.10f * amb.beat));
    c.PopClip();
}

}

void WaveLine(Canvas& c, float a0, float a1, float cy, float amp, double now, Color col, float width, float wavelen) {
    if (a1 <= a0) return;
    if (amp < 0.05f) { // flat: plain line, skip geometry
        c.Line({a0, cy}, {a1, cy}, col, width);
        return;
    }
    const float taper = 16; // edge taper px
    float phase = (float)std::fmod(now * 2 * kPi * 0.75, 2 * kPi); // 0.75 hz scroll, wrapped
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    auto yAt = [&](float px) {
        float k = (std::min)(1.0f, (std::min)((px - a0) / taper, (a1 - px) / taper)); // ramp amp to 0 at both ends
        return cy + amp * (std::max)(0.0f, k) * std::sin(2 * kPi * px / wavelen - phase);
    };
    sink->BeginFigure({a0, yAt(a0)}, D2D1_FIGURE_BEGIN_HOLLOW);
    for (float px = a0 + 2; px < a1; px += 2) sink->AddLine({px, yAt(px)}); // 2px polyline steps
    sink->AddLine({a1, yAt(a1)});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    c.StrokeGeo(path.Get(), col, width); // shared round-cap style + brush, no per-frame com objects
}

void VizGlow(PageCtx& p, const D2D1_RECT_F& r, float scale) {
    if (!p.m.cfg.viz || !p.m.hasTrack) return;
    Ui& ui = p.ui;
    bool playing = p.m.track.playing;
    float left = 0, right = 0;
    if (playing) p.m.meter.Read(left, right);
    float l = ui.Env(Ui::Id("viz.l"), std::sqrt(left));
    float rr = ui.Env(Ui::Id("viz.r"), std::sqrt(right)); // same env ids as DrawViz, shared smoothing
    if (playing || l > 0.01f || rr > 0.01f) ui.KeepAlive();
    if (l < 0.005f && rr < 0.005f) return;

    const Theme& t = p.media;
    Canvas& c = ui.C();
    float w = Width(r), h = Height(r);
    c.PushClip(r);
    c.Glow({r.left + w * 0.22f, r.bottom - h * 0.05f}, w * (0.30f + 0.28f * l) * scale, h * (0.55f + 0.55f * l) * scale, // fixed corners: bottom-left / top-right
           t.accent.Alpha(0.08f + 0.42f * l));
    c.Glow({r.right - w * 0.18f, r.top + h * 0.10f}, w * (0.26f + 0.24f * rr) * scale, h * (0.50f + 0.50f * rr) * scale,
           t.accentHi.Alpha(0.05f + 0.32f * rr));
    c.PopClip();
}

D2D1_RECT_F ArtHover(PageCtx& p, const D2D1_RECT_F& art) { // hover: the cover lifts and a tip offers fullscreen, a click goes there
    if (!p.m.hasTrack) return art;
    uint32_t id = Ui::Id("md.art");
    Ui::Hit h = p.ui.Button(id, art);
    if (h.clicked) p.wantStage = true;
    float lift = p.ui.Smooth(id + 1, h.hover ? 1.0f : 0, 14);
    if (lift > 0.01f) {
        p.hint = L"Fullscreen?";
        p.hintRect = art;
        p.hintFade = lift;
    }
    float g = Width(art) * 0.025f * lift; // very slightly bigger
    return Inset(art, -g, -g);
}

void MediaTall(PageCtx& p) { // sidebar: big centered art, everything else stacked under it
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.media;
    const D2D1_RECT_F& r = p.area;
    const sys::Track& tr = p.m.track;
    bool has = p.m.hasTrack;
    const float radius = 24;
    c.Fill(r, radius, t.card);
    if (p.coverBlur) {
        c.Image(p.coverBlur, r, radius, 0.7f * p.artFade);
        c.FillH(r, radius, t.veil.Alpha(0.30f), t.veil.Alpha(0.72f));
    }
    DrawViz(p, r);
    c.StrokeV(r, radius, t.hairTop, t.hairBot, 1);
    float side = (std::min)(Width(r) - 64, Height(r) * 0.46f);
    D2D1_RECT_F art = ArtHover(p, Rect((r.left + r.right - side) / 2, r.top + 32, side, side));
    c.Fill(Inset(art, -6, -6), 24, Color{0, 0, 0, 0.22f});
    c.Fill(art, 18, t.cardHi);
    if (p.cover) c.Art(p.cover, p.coverBlur, art, 18, p.artFade);
    else c.Icon(glyph::Music, {(art.left + art.right) / 2, (art.top + art.bottom) / 2}, 64, t.textLow);
    float x = r.left + 28, wdt = Width(r) - 56, y = art.bottom + 26;
    c.TextSp(fmt::Upper(has ? tr.app : L"Idle"), Rect(x, y, wdt, 16), Weight::Medium, 10.5f, t.accent, 1.6f, Align::Center);
    c.Text(has && !tr.title.empty() ? tr.title : L"Nothing playing", Rect(x, y + 22, wdt, 36), Weight::Medium, 25, t.text, Align::Center);
    std::wstring sub = !has ? L"Start music or a video in any app" : tr.album.empty() || tr.album == tr.artist ? tr.artist : tr.artist + L"  ·  " + tr.album;
    c.Text(sub, Rect(x, y + 60, wdt, 22), Weight::Light, 16, t.textDim, Align::Center);
    D2D1_RECT_F seek = Rect(x, y + 98, wdt, 28);
    bool known = !has || tr.duration > 0;
    Wave(p, seek, has ? (float)tr.Progress() : 0, has && tr.playing, has && tr.canSeek, known);
    double elapsed = has ? tr.Elapsed() / 1e7 : 0, total = has ? tr.duration / 1e7 : 0;
    if (preview >= 0) elapsed = total * preview;
    if (known) {
        c.Text(fmt::Clock(elapsed), Rect(x, seek.bottom, 80, 18), Weight::Regular, 11.5f, t.textDim);
        c.Text(total > 0 ? fmt::Clock(total) : L"--:--", Rect(seek.right - 80, seek.bottom, 80, 18), Weight::Regular, 11.5f, t.textDim, Align::Right);
    }
    float cy = (std::min)(seek.bottom + 50, r.bottom - 50), cx = (r.left + r.right) / 2; // controls right under the wave
    if (w::IconBtn(ui, t, Ui::Id("md.shuffle"), {cx - 132, cy}, 40, glyph::Shuffle, 22, has && tr.canShuf, has && tr.shuffle)) p.m.media.ToggleShuffle();
    if (w::IconBtn(ui, t, Ui::Id("md.prev"), {cx - 72, cy}, 46, glyph::Prev, 30, has && tr.canPrev)) p.m.media.Previous();
    if (w::PlayBtn(ui, t, Ui::Id("md.play"), {cx, cy}, 60, has && tr.playing, has)) p.m.media.TogglePlay();
    if (w::IconBtn(ui, t, Ui::Id("md.next"), {cx + 72, cy}, 46, glyph::Next, 30, has && tr.canNext)) p.m.media.Next();
    if (w::IconBtn(ui, t, Ui::Id("md.repeat"), {cx + 132, cy}, 40, has && tr.repeat == 1 ? glyph::RepeatOne : glyph::Repeat, 22, has && tr.canRep,
                   has && tr.repeat != 0))
        p.m.media.CycleRepeat();
}

void DrawMedia(PageCtx& p) {
    if (Height(p.area) > Width(p.area)) {
        MediaTall(p);
        return;
    }
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.media;
    const D2D1_RECT_F& r = p.area;
    const sys::Track& tr = p.m.track;
    bool has = p.m.hasTrack;
    const float radius = 24;

    c.Fill(r, radius, t.card);
    if (p.coverBlur) {
        c.Image(p.coverBlur, r, radius, 0.7f * p.artFade);
        c.FillH(r, radius, t.veil.Alpha(0.30f), t.veil.Alpha(0.72f)); // wash for legibility
    }
    float side = Height(r) - 52; // square art, 26px margin top/bottom
    float x = r.left + 26 + side + 30, wdt = r.right - 28 - x; // text column right of art
    DrawViz(p, r); // under border + art, over backdrop
    c.StrokeV(r, radius, t.hairTop, t.hairBot, 1);

    D2D1_RECT_F art = ArtHover(p, Rect(r.left + 26, r.top + 26, side, side));
    c.Fill(Inset(art, -6, -6), 24, Color{0, 0, 0, 0.22f}); // soft shadow ring behind art
    c.Fill(art, 18, t.cardHi);
    if (p.cover) c.Art(p.cover, p.coverBlur, art, 18, p.artFade);
    else c.Icon(glyph::Music, {(art.left + art.right) / 2, (art.top + art.bottom) / 2}, 64, t.textLow);
    const float block = 210; // label, title, artist, wave, controls: one tight stack, centered beside the art
    float y0 = (std::max)(r.top + 26, (r.top + r.bottom - block) / 2);
    c.TextSp(fmt::Upper(has ? tr.app : L"Idle"), Rect(x, y0, wdt, 16), Weight::Medium, 10.5f, t.accent, 1.6f);
    c.Text(has && !tr.title.empty() ? tr.title : L"Nothing playing", Rect(x, y0 + 22, wdt, 44), Weight::Medium, 31, t.text);
    std::wstring sub = !has ? L"Start music or a video in any app" :
                       tr.album.empty() || tr.album == tr.artist ? tr.artist : tr.artist + L"  ·  " + tr.album;
    c.Text(sub, Rect(x, y0 + 66, wdt, 26), Weight::Light, 17, t.textDim);

    D2D1_RECT_F seek = Rect(x, y0 + 102, wdt, 28); // the wave right under the artist
    float cy = seek.bottom + 48, cx = x + wdt / 2; // controls right under the wave, centered in text column, 60/72px steps
    bool known = !has || tr.duration > 0; // some apps (firefox-based browsers) never report length or position
    Wave(p, seek, has ? (float)tr.Progress() : 0, has && tr.playing, has && tr.canSeek, known);
    double elapsed = has ? tr.Elapsed() / 1e7 : 0, total = has ? tr.duration / 1e7 : 0; // 100ns units -> seconds
    if (preview >= 0) elapsed = total * preview; // clock follows drag preview
    if (known) { // no clock beats a made-up one
        c.Text(fmt::Clock(elapsed), Rect(x, seek.bottom, 80, 18), Weight::Regular, 11.5f, t.textDim);
        c.Text(total > 0 ? fmt::Clock(total) : L"--:--", Rect(seek.right - 80, seek.bottom, 80, 18), Weight::Regular, 11.5f,
               t.textDim, Align::Right);
    }

    if (w::IconBtn(ui, t, Ui::Id("md.shuffle"), {cx - 132, cy}, 40, glyph::Shuffle, 22, has && tr.canShuf, has && tr.shuffle))
        p.m.media.ToggleShuffle();
    if (w::IconBtn(ui, t, Ui::Id("md.prev"), {cx - 72, cy}, 46, glyph::Prev, 30, has && tr.canPrev)) p.m.media.Previous();
    if (w::PlayBtn(ui, t, Ui::Id("md.play"), {cx, cy}, 60, has && tr.playing, has)) p.m.media.TogglePlay();
    if (w::IconBtn(ui, t, Ui::Id("md.next"), {cx + 72, cy}, 46, glyph::Next, 30, has && tr.canNext)) p.m.media.Next();
    bool repeating = has && tr.repeat != 0;
    if (w::IconBtn(ui, t, Ui::Id("md.repeat"), {cx + 132, cy}, 40, has && tr.repeat == 1 ? glyph::RepeatOne : glyph::Repeat, 22,
                      has && tr.canRep, repeating))
        p.m.media.CycleRepeat();
}
