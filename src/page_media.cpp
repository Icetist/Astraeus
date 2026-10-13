#include <algorithm>
#include <cmath>

#include "anim.h"
#include "glyphs.h"
#include "pages.h"

namespace {

constexpr float kPi = 3.14159265f;
float preview = -1; // seek preview while dragging, -1 = none

void Wave(PageCtx& p, const D2D1_RECT_F& r, float fraction, bool playing, bool canSeek) {
    Ui& ui = p.ui;
    Canvas& c = ui.C();
    const Theme& t = p.media;
    uint32_t id = Ui::Id("md.seek");
    float v = preview >= 0 ? preview : fraction;
    if (canSeek && ui.Slider(id, r, v, false)) preview = v; // preview seek while dragging
    if (ui.IsActive(id) && ui.In().released) { // commit seek on release only
        p.m.media.Seek(v);
        preview = -1;
    }
    if (!ui.IsActive(id)) preview = -1;

    float amp = ui.Smooth(id + 1, playing && !ui.IsActive(id) ? 3.2f : 0, 5); // wave flattens when paused or dragging
    float cy = (r.top + r.bottom) / 2;
    float x = r.left + Width(r) * anim::Clamp01(v);
    const float gap = 7, taper = 16, wave = 28; // gap around head, edge taper, wavelength px
    float phase = (float)std::fmod(ui.Now() * 2 * kPi * 0.75, 2 * kPi); // 0.75 hz scroll, wrapped

    float a0 = r.left + 2, a1 = x - gap; // played part, left of head
    if (a1 > a0) {
        if (amp < 0.05f) c.Line({a0, cy}, {a1, cy}, t.accent, 4); // flat: plain line, skip geometry
        else {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(gfx::D2D()->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                auto yAt = [&](float px) {
                    float k = (std::min)(1.0f, (std::min)((px - a0) / taper, (a1 - px) / taper)); // ramp amp to 0 at both ends
                    return cy + amp * (std::max)(0.0f, k) * std::sin(2 * kPi * px / wave - phase);
                };
                sink->BeginFigure({a0, yAt(a0)}, D2D1_FIGURE_BEGIN_HOLLOW);
                for (float px = a0 + 2; px < a1; px += 2) sink->AddLine({px, yAt(px)}); // 2px polyline steps
                sink->AddLine({a1, yAt(a1)});
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                ComPtr<ID2D1SolidColorBrush> ink;
                ComPtr<ID2D1StrokeStyle> round;
                D2D1_STROKE_STYLE_PROPERTIES sp = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                   D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0};
                gfx::D2D()->CreateStrokeStyle(sp, nullptr, 0, &round);
                if (SUCCEEDED(c.Target()->CreateSolidColorBrush(t.accent.D2D(), &ink)))
                    c.Target()->DrawGeometry(path.Get(), ink.Get(), 4, round.Get());
            }
            ui.KeepAlive(); // keep repainting while wave moves
        }
    }
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

void DrawMedia(PageCtx& p) {
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
        c.FillH(r, radius, Color{0, 0, 0, 0.30f}, Color{0, 0, 0, 0.72f}); // darken for legibility
    }
    float side = Height(r) - 52; // square art, 26px margin top/bottom
    float x = r.left + 26 + side + 30, wdt = r.right - 28 - x; // text column right of art
    DrawViz(p, r); // under border + art, over backdrop
    c.StrokeV(r, radius, t.hairTop, t.hairBot, 1);

    D2D1_RECT_F art = Rect(r.left + 26, r.top + 26, side, side);
    c.Fill(Inset(art, -6, -6), 24, Color{0, 0, 0, 0.22f}); // soft shadow ring behind art
    c.Fill(art, 18, t.cardHi);
    if (p.cover) c.Image(p.cover, art, 18, p.artFade);
    else c.Icon(glyph::Music, {(art.left + art.right) / 2, (art.top + art.bottom) / 2}, 64, t.textLow);
    c.TextSp(fmt::Upper(has ? tr.app : L"Idle"), Rect(x, r.top + 30, wdt, 16), Weight::Medium, 10.5f, t.accent, 1.6f);
    c.Text(has && !tr.title.empty() ? tr.title : L"Nothing playing", Rect(x, r.top + 58, wdt, 38), Weight::Medium, 27, t.text);
    std::wstring sub = !has ? L"Start music or a video in any app" :
                       tr.album.empty() || tr.album == tr.artist ? tr.artist : tr.artist + L"  ·  " + tr.album;
    c.Text(sub, Rect(x, r.top + 98, wdt, 22), Weight::Light, 15, t.textDim);

    D2D1_RECT_F seek = Rect(x, r.top + 168, wdt, 28);
    Wave(p, seek, has ? (float)tr.Progress() : 0, has && tr.playing, has && tr.canSeek);
    double elapsed = has ? tr.Elapsed() / 1e7 : 0, total = has ? tr.duration / 1e7 : 0; // 100ns units -> seconds
    if (preview >= 0) elapsed = total * preview; // clock follows drag preview
    c.Text(fmt::Clock(elapsed), Rect(x, seek.bottom, 80, 18), Weight::Regular, 11.5f, t.textDim);
    c.Text(total > 0 ? fmt::Clock(total) : L"--:--", Rect(seek.right - 80, seek.bottom, 80, 18), Weight::Regular, 11.5f,
           t.textDim, Align::Right);

    float cy = r.bottom - 50, cx = x + wdt / 2; // controls centered in text column, 60/72px steps
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
