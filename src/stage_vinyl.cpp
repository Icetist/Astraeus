// The fullscreen player's vinyl room: the cover is the record's sleeve, and the record slides out of it to play,
// turning under an arm that drops onto it; on pause the arm lifts and the record slips back in.
// The record is the control: a click plays or stops it, and turning it by hand seeks. Under it, centred: the song and the line being sung.
// The light on the record is the room's, so it never turns with it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "anim.h"
#include "glyphs.h"
#include "pages.h"
#include "stage.h"
#include "sys_sound.h"

namespace {

constexpr double kRpmDeg = 200; // 33 1/3 turns a minute, in degrees a second
constexpr float kSpinUp = 1.6f, kSpinDown = 0.7f; // a platter comes up to speed quicker than it coasts down
constexpr float kSlideRate = 1.8f, kArmRate = 2.2f, kCalmRate = 20; // the record out of its sleeve, the arm's swing; with reduced motion they're just there
constexpr float kArmWaits = 0.97f, kArmLanded = 0.98f; // the arm drops only once the record is all the way out; past this it's on the groove
constexpr float kSleeveH = 0.42f, kSleeveW = 0.28f; // the sleeve's side: a share of the screen's height, or its width when that's tighter
constexpr float kSleeveAt = 0.44f; // the pair's middle down the screen
constexpr float kCtlRise = 0.04f, kCtlShrink = 0.08f; // with the buttons on: higher by this share of the screen, smaller by this share of itself
constexpr float kDisc = 0.94f; // the record a little smaller than its sleeve, as they are
constexpr float kOutRest = 0.24f, kOutPlay = 0.52f; // how far the record's middle sits right of the sleeve's: peeking out enough to click, and out to play
constexpr float kSongGap = 48; // dips from the sleeve down to the song
constexpr float kCtlGap = 150; // and on from the song's top to the seek line, where the buttons sit
constexpr float kWearR = 0.43f; // the faint ring the record has pressed into the sleeve
constexpr float kLabelR = 0.30f, kSpindleR = 0.028f, kRimIn = 0.965f; // shares of the record's radius
constexpr int kGrooves = 32; // rings across the playing surface: enough to read as grooves, few enough to cost nothing
constexpr float kGrooveIn = 0.41f, kGrooveOut = 0.95f; // between the label and the grooves: the plain band the song's name is pressed into
constexpr float kTrackGaps[] = {0.54f, 0.67f, 0.80f}; // the plain bands between songs on a side
constexpr float kSheenTilt = -38, kSheenSway = 3; // degrees: the two bands of light across it, as a lamp over a turntable leaves them; a pressing is never quite flat
constexpr float kLabelType = 0.55f, kLabelSpace = 0.18f, kLabelArc = 320; // the pressed name: a share of its band, letter spacing a share of the type, at most this much of the circle
constexpr float kArmLen = 1.22f, kPivotX = 1.06f, kPivotY = -0.80f, kPivotR = 0.085f; // the arm from its pivot, up and right of the record; the pivot's own size
constexpr float kRestDeg = 82; // resting: hanging just clear of the record's edge
constexpr float kPlayOut = 0.92f, kPlayIn = 0.46f; // the stylus works inward as the song goes on
constexpr float kScrubStart = 6; // dips the hand moves before a press on the record turns it instead of clicking
constexpr double kScrubTurnSec = 12; // a whole turn by hand moves the song this far
constexpr double kScrubHoldSec = 1.2; // after letting go, the arm keeps its place this long while the player catches up
constexpr float kLineFade = 0.45f; // a new sung line fades in this long
constexpr double kLineHold = 1.5; // and stays this long past its singing, then the gap is quiet
constexpr float kPi = 3.14159265f;

float Ease(float v) { return v * v * (3 - 2 * v); }

void Follow(float& v, float to, float rate, float dt) { v += (to - v) * (std::min)(1.0f, dt * rate); }

}

void Stage::VinylStep() {
    bool playing = model.hasTrack && model.track.playing;
    float dt = frameDt;
    bool calm = model.reduceMotion != 0;
    Follow(vinylSlide, playing ? 1.0f : 0, calm ? kCalmRate : kSlideRate, dt);
    Follow(vinylSpin, playing ? 1.0f : 0, playing ? kSpinUp : kSpinDown, dt);
    if (!playing && vinylSpin < 0.002f) vinylSpin = 0;
    if (!vinylScrub) vinylAngle = std::fmod(vinylAngle + kRpmDeg * model.stageVinylSpeed / 100.0 * vinylSpin * dt, 360.0); // the editor's spin speed, a share of the real thing; a hand on it holds it
    Follow(vinylArm, playing && vinylSlide > kArmWaits ? 1.0f : 0, calm ? kCalmRate : kArmRate, dt);
    bool landed = vinylArm > kArmLanded;
    if (landed && !vinylLanded && model.stageVinylSound && model.stageVinyl) // once a landing, never on its way up
        sound::Play(sound::Cue::Needle);
    vinylLanded = landed;
}

void Stage::VinylLabelText(D2D1_POINT_2F mid, float radius, float k) {
    const sys::Track& tr = model.track;
    if (!model.hasTrack || tr.title.empty()) return;
    Canvas& c = *canvas;
    float R = radius / 2, band = R * (kGrooveIn - kLabelR), size = (std::max)(7.0f * k, band * kLabelType), at = R * kLabelR + band / 2, gap = size * kLabelSpace;
    std::wstring text = fmt::Upper(tr.title) + (tr.artist.empty() ? L"" : L"   ·   " + fmt::Upper(tr.artist));
    float widths[128] = {}, total = 0, most = kLabelArc * kPi / 180 * at;
    size_t n = 0;
    for (; n < text.size() && n < 128; ++n) { // as much as fits around: a long title stops short of meeting itself
        float w = c.Measure(text.substr(n, 1), Weight::Medium, size) + gap;
        if (total + w > most) break;
        widths[n] = w, total += w;
    }
    const Color ink = {1, 1, 1, 0.42f};
    D2D1_MATRIX_3X2_F was;
    ctx->GetTransform(&was);
    float along = -total / 2; // centred over the top of the label, then turning with it
    for (size_t i = 0; i < n; ++i) {
        float deg = (float)vinylAngle + (along + widths[i] / 2) / at * 180 / kPi;
        along += widths[i];
        if (text[i] == L' ') continue;
        ctx->SetTransform(D2D1::Matrix3x2F::Rotation(deg, mid) * was);
        c.Text(text.substr(i, 1), Rect(mid.x - widths[i], mid.y - at - size * 0.75f, 2 * widths[i], size * 1.5f), Weight::Medium, size, ink, Align::Center);
    }
    ctx->SetTransform(was);
}

void Stage::VinylDisc(D2D1_POINT_2F mid, float side, float k) {
    Canvas& c = *canvas;
    float R = side / 2;
    D2D1_RECT_F disc = Rect(mid.x - R, mid.y - R, side, side);
    const D2D1_ROUNDED_RECT round = {disc, R, R};
    bool fine = model.stageVinylLabel != 0; // the finer touches: the pressed name, the light's sway
    c.Fill(disc, R, Color{0.035f, 0.035f, 0.04f, 1}.Mix(look.accent, 0.08f)); // near black, warmed a touch by the album's color
    for (int i = 0; i < kGrooves; ++i) {
        float r = R * (kGrooveIn + (kGrooveOut - kGrooveIn) * i / (kGrooves - 1));
        c.Ring(mid, r, Color{1, 1, 1, i % 4 == 0 ? 0.045f : 0.022f}, 1);
    }
    for (float g : kTrackGaps) c.Ring(mid, R * g, Color{0, 0, 0, 0.55f}, 2.5f * k);

    D2D1_MATRIX_3X2_F was;
    ctx->GetTransform(&was);
    float tilt = kSheenTilt + (fine ? kSheenSway * std::sin((float)vinylAngle * kPi / 180) : 0); // the light drifts a little as it turns
    ctx->SetTransform(D2D1::Matrix3x2F::Rotation(tilt, mid) * was);
    float sheen = LightRoom() ? 0.10f : 0.075f;
    c.Glow({mid.x, mid.y - R * 0.55f}, R * 0.20f, R * 0.62f, Color{1, 1, 1, sheen}, &round);
    c.Glow({mid.x, mid.y + R * 0.55f}, R * 0.20f, R * 0.62f, Color{1, 1, 1, sheen * 0.8f}, &round);
    c.Glow({mid.x, mid.y - R * 0.55f}, R * 0.34f, R * 0.70f, look.accentHi.Alpha(0.06f), &round); // the album's color in the shine
    ctx->SetTransform(was);
    c.Ring(mid, R * kRimIn, Color{1, 1, 1, 0.05f}, 1);
    c.Ring(mid, R - 0.5f, Color{1, 1, 1, 0.12f}, 1);

    if (fine) VinylLabelText(mid, side, k);
    float lr = R * kLabelR;
    D2D1_RECT_F label = Rect(mid.x - lr, mid.y - lr, 2 * lr, 2 * lr);
    ctx->SetTransform(D2D1::Matrix3x2F::Rotation((float)vinylAngle, mid) * was); // only the label shows the turning: grooves are round
    if (cover) {
        c.Art(cover.Get(), coverBlur.Get(), label, lr, 1);
    } else { // no art: a plain label in the album's color, with a mark so it can be seen to turn
        c.FillV(label, lr, look.accentHi.Alpha(0.9f), look.accent.Alpha(0.7f));
        c.Dot({mid.x, mid.y - lr * 0.62f}, lr * 0.07f, Color{1, 1, 1, 0.55f});
    }
    ctx->SetTransform(was);
    c.Ring(mid, lr, Color{0, 0, 0, 0.45f}, 2 * k); // the label's pressed edge
    c.Ring(mid, lr + 3 * k, Color{1, 1, 1, 0.06f}, 1);
    c.Dot(mid, R * kSpindleR, Color{0.80f, 0.80f, 0.83f, 1}); // the spindle
    c.Dot(mid, R * kSpindleR * 0.45f, Color{0.35f, 0.35f, 0.38f, 1});
}

void Stage::VinylArm(D2D1_POINT_2F mid, float side, float k) { // where the stylus meets a groove is found from the pivot, so it lands right at any size
    Canvas& c = *canvas;
    const sys::Track& tr = model.track;
    float R = side / 2;
    D2D1_POINT_2F P = {mid.x + R * kPivotX, mid.y + R * kPivotY};
    float L = R * kArmLen, dx = P.x - mid.x, dy = P.y - mid.y, d = std::hypot(dx, dy), phi = std::atan2(dy, dx);
    double len = tr.duration / 1e7;
    bool byHand = len > 0 && (vinylScrub || ui.Now() - vinylScrubEnd < kScrubHoldSec); // the hand's place, not the player's, until the player has caught up
    float prog = !model.hasTrack ? 0 : byHand ? (float)(vinylScrubAt / len) : (float)tr.Progress();
    prog = (std::clamp)(prog, 0.0f, 1.0f);
    float rp = R * (kPlayOut + (kPlayIn - kPlayOut) * prog);
    float play = phi + std::acos(std::clamp((rp * rp - d * d - L * L) / (2 * L * d), -1.0f, 1.0f)), rest = kRestDeg * kPi / 180;
    float on = Ease(vinylArm), th = rest + (play - rest) * on;
    D2D1_POINT_2F u = {std::cos(th), std::sin(th)}, S = {P.x + u.x * L, P.y + u.y * L};
    Color metal = LightRoom() ? Color{0.30f, 0.30f, 0.33f, 1} : Color{0.82f, 0.82f, 0.86f, 1}, dark = Color{0.12f, 0.12f, 0.14f, 1};
    float off = 6 * k; // its shadow, a little down and right
    c.Line({P.x + off, P.y + off}, {S.x + off, S.y + off}, Color{0, 0, 0, 0.35f}, 6 * k);
    c.Line(P, {P.x - u.x * R * 0.20f, P.y - u.y * R * 0.20f}, dark, 13 * k); // the counterweight behind the pivot
    c.Line(P, S, metal, 4 * k);
    D2D1_POINT_2F shellA = {S.x - u.x * R * 0.09f, S.y - u.y * R * 0.09f}, shellB = {S.x + u.x * R * 0.03f, S.y + u.y * R * 0.03f};
    c.Line(shellA, shellB, dark, 10 * k); // the headshell
    c.Line(shellA, shellB, metal.Alpha(0.35f), 2 * k);
    c.Dot(S, 2.2f * k, look.accentHi.Alpha(0.4f + 0.6f * on)); // the stylus, lit while it plays
    c.Dot(P, R * kPivotR, dark); // the pivot
    c.Ring(P, R * kPivotR, Color{1, 1, 1, 0.18f}, 1);
    c.Dot(P, R * 0.035f, metal);
}

void Stage::VinylScrub(const D2D1_RECT_F& discR, D2D1_POINT_2F disc, uint32_t id) {
    const Input& in = ui.In();
    const sys::Track& tr = model.track;
    (void)discR;
    if (in.pressed) vinylScrubbed = false; // a new press is a click until it turns
    bool held = in.down && ui.IsActive(id);
    double len = tr.duration / 1e7;
    bool can = model.hasTrack && tr.canSeek && len > 0;
    float ang = std::atan2(in.mouse.y - disc.y, in.mouse.x - disc.x);
    if (held && can && !vinylScrub && std::hypot(in.mouse.x - in.pressPos.x, in.mouse.y - in.pressPos.y) > kScrubStart) {
        vinylScrub = vinylScrubbed = true;
        vinylScrubLast = ang;
        vinylScrubAt = tr.Elapsed() / 1e7;
    }
    if (vinylScrub && held) { // clockwise is forward, as the record turns
        float d = ang - vinylScrubLast;
        if (d > kPi) d -= 2 * kPi;
        else if (d < -kPi) d += 2 * kPi;
        vinylScrubLast = ang;
        vinylAngle = std::fmod(vinylAngle + d * 180 / kPi + 360.0, 360.0);
        vinylScrubAt = std::clamp(vinylScrubAt + d / (2 * kPi) * kScrubTurnSec, 0.0, len);
        ui.Wake();
    } else if (vinylScrub) { // let go: the song goes on from where the hand left it
        vinylScrub = false;
        vinylScrubEnd = ui.Now();
        if (can) model.media.Seek(vinylScrubAt / len);
    }
}

void Stage::VinylLine(float cx, float y, float w, float k) {
    if (!lyrDoc || lyrDoc->lines.empty() || !model.fullLyrics || !model.hasTrack) {
        vinylLine = -1;
        return;
    }
    const auto& lines = lyrDoc->lines;
    double at = model.track.Elapsed() / 1e7 - model.lyricOffsetMs / 1000.0; // [ and ] fine-tune it, as with the full lyrics
    auto it = std::upper_bound(lines.begin(), lines.end(), at, [](double t, const sys::LyricLine& l) { return t < l.start; });
    int n = (int)(it - lines.begin()) - 1;
    if (n >= 0 && at > lines[n].end + kLineHold) n = -1; // between verses: nothing
    double now = ui.Now();
    if (n != vinylLine) vinylLine = n, vinylLineAt = now;
    if (n < 0 || lines[n].text.empty()) return;
    float in = anim::Clamp01((float)(now - vinylLineAt) / kLineFade);
    if (in < 1) ui.Wake();
    canvas->Text(lines[n].text, Rect(cx - w / 2, y + 4 * k * (1 - in), w, 28 * k), Weight::Light, 18 * k, look.textDim.Alpha(0.85f * in), Align::Center);
}

void Stage::VinylPlayer(float show) {
    VinylStep();
    Canvas& c = *canvas;
    const sys::Track& tr = model.track;
    bool has = model.hasTrack;
    const StagePrefs::Shape& s = shapeNow[0]; // arranged as the layout without lyrics: the words here are only ever one line
    float k = K();
    float ctl = ui.Smooth(Ui::Id("st.v.ctl"), model.stageVinylCtl ? 1.0f : 0, 8); // the buttons coming in: the record rises and shrinks a little to make their room
    float S0 = (std::min)(H * kSleeveH, W * kSleeveW) * (1 - kCtlShrink * ctl), atY = H * (kSleeveAt - kCtlRise * ctl);
    float S = S0 * s.coverS, D = S * kDisc;
    float reach = kOutPlay * S + D / 2 * (kPivotX + kPivotR); // the record out to play and its arm's pivot: the pair's right edge, right of the sleeve's middle
    D2D1_POINT_2F mid = {W / 2 + s.coverX * W, atY + s.coverY * H}; // the middle of all of it, sleeve to arm: what the editor's guides line up
    D2D1_POINT_2F sleeve = {mid.x - (reach - S / 2) / 2, mid.y}, disc = {sleeve.x + S * (kOutRest + (kOutPlay - kOutRest) * Ease(vinylSlide)), mid.y};
    D2D1_RECT_F sl = Rect(sleeve.x - S / 2, mid.y - S / 2, S, S);
    float rad = 3 * k;

    c.Glow({mid.x + S * 0.1f, mid.y + S * 0.08f}, S * 1.4f, S * 0.9f, look.accent.Alpha((0.22f + 0.22f * beat) * s.coverGlow)); // light spilling from the pair
    c.Glow({mid.x, sl.bottom + 6 * k}, S * 1.1f, S * 0.14f, Color{0, 0, 0, LightRoom() ? 0.16f : 0.42f}); // and their shadow
    VinylDisc(disc, D, k); // behind the sleeve: what's still inside stays hidden by it
    c.Fill(Rect(sl.left + 3 * k, sl.top + 3 * k, S, S), rad, Color{0, 0, 0, 0.30f}); // the sleeve's own edge, lifted off the record
    Ui::Hit hit = ui.Button(Ui::Id("st.v.sleeve"), sl);
    if (hit.clicked) Leave(); // the cover brought you here, the cover takes you back
    if (cover) c.Art(cover.Get(), coverBlur.Get(), sl, rad, 1);
    else c.FillV(sl, rad, look.accentHi.Alpha(0.85f), look.accent.Alpha(0.55f));
    const D2D1_ROUNDED_RECT paper = {sl, rad, rad};
    if (model.stageVinylLabel) c.FillV(sl, rad, Color{1, 1, 1, 0.035f}, Color{0, 0, 0, 0.07f}); // card, not glass: lighter where the lamp reaches it
    c.Ring(sleeve, S * kWearR, Color{1, 1, 1, 0.05f}, 1.5f * k); // where the record inside has pressed for years
    c.Glow({sl.left + S * 0.25f, sl.top + S * 0.2f}, S * 0.45f, S * 0.3f, Color{1, 1, 1, 0.05f}, &paper); // light on the paper
    c.Stroke(sl, rad, Color{1, 1, 1, 0.10f}, 1);
    c.Line({sl.right, sl.top + 4 * k}, {sl.right, sl.bottom - 4 * k}, Color{0, 0, 0, 0.5f}, 2 * k); // the opening the record comes out of
    VinylArm(disc, D, k);
    if (float tip = ui.Smooth(Ui::Id("st.v.sleeve", 1), hit.hover ? 1.0f : 0, 14); tip > 0.01f) { // as the cover does: say what a click does
        float pw = 150 * k, ph = 32 * k;
        D2D1_RECT_F pill = Rect(sleeve.x - pw / 2, sl.bottom - ph - 14 * k + 4 * (1 - tip), pw, ph);
        Color fill = look.bg;
        fill.a = 0.90f;
        c.Fill(pill, ph / 2, fill.Alpha(tip));
        c.StrokeV(pill, ph / 2, look.hairTop.Alpha(tip), look.hairBot.Alpha(tip), 1);
        c.Text(L"Close fullscreen?", pill, Weight::Medium, 13 * k, look.text.Alpha(tip), Align::Center);
    }
    partCover = {sl.left, sl.top, sleeve.x + reach, sl.bottom}; // as far as it ever reaches: the box holds still while the record slides

    // the record is the control, as on a turntable: a click plays or stops it, turning it by hand seeks
    D2D1_RECT_F discR = Rect(disc.x - D / 2, disc.y - D / 2, D, D);
    uint32_t discId = Ui::Id("st.v.disc");
    Ui::Hit turn = ui.Button(discId, discR); // after the sleeve's: where they overlap, the sleeve takes the click
    ui.Name(has && tr.playing ? L"Pause" : L"Play");
    VinylScrub(discR, disc, discId);
    if (turn.clicked && has && !vinylScrubbed) model.media.TogglePlay();
    std::wstring say = vinylScrub ? fmt::Clock(vinylScrubAt) + L" / " + fmt::Clock(tr.duration / 1e7) : tr.playing ? L"Stop" : L"Play";
    if (float tip = ui.Smooth(Ui::Id("st.v.disc", 1), (turn.hover || vinylScrub) && has ? 1.0f : 0, 14); tip > 0.01f) {
        float pw = (std::max)(96 * k, c.Measure(say, Weight::Medium, 13 * k) + 32 * k), ph = 32 * k, px = (std::max)(sl.right + 12 * k, disc.x + D * 0.1f);
        D2D1_RECT_F pill = Rect(px, discR.bottom - ph - 14 * k + 4 * (1 - tip), pw, ph);
        Color fill = look.bg;
        fill.a = 0.90f;
        c.Fill(pill, ph / 2, fill.Alpha(tip));
        c.StrokeV(pill, ph / 2, look.hairTop.Alpha(tip), look.hairBot.Alpha(tip), 1);
        c.Text(say, pill, Weight::Medium, 13 * k, look.text.Alpha(tip), Align::Center);
    }

    float tk = k * s.textS, cx = W / 2 + s.textX * W, ty = atY + S0 / 2 + kSongGap * k + s.textY * H, tw = (std::min)(W * 0.6f, 900 * k); // its own place on the screen: moving the record leaves the song where it is
    std::wstring title = has && !tr.title.empty() ? tr.title : L"Nothing playing";
    std::wstring sub = !has ? L"Start music or a video in any app" : tr.album.empty() || tr.album == tr.artist || s.albumOn < 0.5f ? tr.artist : tr.artist + L"  ·  " + tr.album;
    c.Text(title, Rect(cx - tw / 2, ty, tw, 50 * tk), Weight::Medium, 38 * tk, look.text, Align::Center);
    c.Text(sub, Rect(cx - tw / 2, ty + 50 * tk, tw, 30 * tk), Weight::Light, 20 * tk, look.textDim, Align::Center);
    VinylLine(cx, ty + 92 * tk, tw, tk);
    partText = Rect(cx - tw / 2, ty, tw, 124 * tk);
    // the buttons: a part of their own, under the song's resting place, moved and sized apart from it
    float bk = k * s.ctlS, ck = bk * 0.85f, bx = W / 2 + s.ctlX * W, sy = atY + S0 / 2 + (kSongGap + kCtlGap) * k + s.ctlY * H, y = sy + 66 * bk;
    float sw = (std::min)(W * 0.36f, 600 * k) * s.ctlS, half = (std::max)(sw / 2, 230 * ck);
    partCtl = {bx - half, sy - 18 * bk, bx + half, y + 42 * ck};
    float a = show * ctl;
    if (a <= 0.01f) return; // off, or hidden at rest
    bool layer = a < 0.999f;
    if (layer) ctx->PushLayer(D2D1::LayerParameters1(Inset(partCtl, -20 * ck, -20 * ck), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    timesNow = s.timesOn;
    Seek(Rect(bx - sw / 2, sy - 16 * bk, sw, 32 * bk), show);
    const Theme ctlTheme = theme::Lift(look); // the play button leans toward the ink, so it stands clear of the light behind it
    if (w::IconBtn(ui, ctlTheme, Ui::Id("st.v.prev"), {bx - 100 * ck, y}, 60 * ck, glyph::Prev, 38 * ck, has && tr.canPrev)) model.media.Previous();
    if (w::PlayBtn(ui, ctlTheme, Ui::Id("st.v.play"), {bx, y}, 84 * ck, has && tr.playing, has)) model.media.TogglePlay();
    if (w::IconBtn(ui, ctlTheme, Ui::Id("st.v.next"), {bx + 100 * ck, y}, 60 * ck, glyph::Next, 38 * ck, has && tr.canNext)) model.media.Next();
    if (s.extrasOn > 0.5f) {
        D2D1_POINT_2F shuf = {bx - 196 * ck, y}, rep = {bx + 196 * ck, y};
        if (w::IconBtn(ui, ctlTheme, Ui::Id("st.v.shuffle"), shuf, 48 * ck, glyph::Shuffle, 26 * ck, has && tr.canShuf, has && tr.shuffle)) model.media.ToggleShuffle();
        if (w::IconBtn(ui, ctlTheme, Ui::Id("st.v.repeat"), rep, 48 * ck, has && tr.repeat == 1 ? glyph::RepeatOne : glyph::Repeat, 26 * ck, has && tr.canRep, has && tr.repeat != 0))
            model.media.CycleRepeat();
        float sOn = ui.Smooth(Ui::Id("st.v.shuffle", 7), has && tr.shuffle ? 1.0f : 0, 14), rOn = ui.Smooth(Ui::Id("st.v.repeat", 7), has && tr.repeat != 0 ? 1.0f : 0, 14);
        if (sOn > 0.01f) c.Dot({shuf.x, shuf.y + 22 * ck}, 2.8f * ck * sOn, ctlTheme.accent.Alpha(sOn)); // on: a dot under it, not just a color change
        if (rOn > 0.01f) c.Dot({rep.x, rep.y + 22 * ck}, 2.8f * ck * rOn, ctlTheme.accent.Alpha(rOn));
    }
    if (layer) ctx->PopLayer();
}
