// the settings previews: a small see-through pill by the mouse, a sketch of what the hovered switch does
#include "preview.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "glyphs.h"

namespace preview {
namespace {

constexpr float kDelay = 0.35f; // seconds hovered before it shows: passing over doesn't flash it
constexpr float kW = 236, kH = 112, kPad = 12, kOff = 18; // the pill, its inner margin, how far it sits from the mouse
constexpr float kFade = 14; // how quickly it comes and goes
constexpr float kSketchAt = 0.5f; // the sketch shows once the pill is this far in: never a faint card with a full drawing on it

int cur = kNone, shown = kNone; // under the mouse now; the last one drawn, kept while it fades
double since = 0;
bool seen = false; // some control claimed the mouse this frame

const Color kInk{0.95f, 0.95f, 0.97f, 1}, kNight{0.06f, 0.06f, 0.08f, 0.92f}, kCoverA{1.0f, 0.42f, 0.62f, 1}, kCoverB{0.42f, 0.32f, 0.86f, 1};

struct Sketch { // the drawing tools, inside the pill
    Canvas& c;
    const Theme& t;
    D2D1_RECT_F in;
    double now;
    float cx() const { return (in.left + in.right) / 2; }
    float cy() const { return (in.top + in.bottom) / 2; }
    void Line(float x, float y, float w, Color col, float h = 6) { c.Fill({x, y, x + w, y + h}, h / 2, col); } // a line of words, as a bar
    void Cover(const D2D1_RECT_F& r) { c.FillV(r, (std::min)(r.right - r.left, r.bottom - r.top) * 0.2f, kCoverA, kCoverB); }
    void Bars(float x, float base, float h, Color col, int n = 4, float w = 3) { // moving, like the music's
        for (int i = 0; i < n; ++i) {
            float k = 0.35f + 0.65f * (0.5f + 0.5f * (float)std::sin(now * (5.0 + i * 1.7) + i));
            c.Fill({x + i * (w + 2), base - h * k, x + i * (w + 2) + w, base}, w / 2, col);
        }
    }
    D2D1_RECT_F Pill(float w, float h, Color fill, float y = -1) { // a long or small pill, centred
        float top = y < 0 ? cy() - h / 2 : y;
        D2D1_RECT_F r = {cx() - w / 2, top, cx() + w / 2, top + h};
        c.Fill(r, h / 2, fill);
        return r;
    }
    void Edge(float y) { c.Line({in.left, y}, {in.right, y}, t.text.Alpha(0.35f), 1); } // the screen's top edge
    void Words(const std::wstring& s, const D2D1_RECT_F& r, float size, Color col, Align a = Align::Left, Weight w = Weight::Medium) { c.Text(s, r, w, size, col, a); }
};

void Paint(Sketch& s, int kind) {
    Canvas& c = s.c;
    const Theme& t = s.t;
    D2D1_RECT_F in = s.in;
    float cx = s.cx(), cy = s.cy();
    Color dim = t.text.Alpha(0.35f), faint = t.text.Alpha(0.18f);
    switch (kind) {
    case kCompact: { // a smaller cover beside the song
        D2D1_RECT_F card = {in.left + 8, in.top + 14, in.right - 8, in.bottom - 14};
        c.Fill(card, 12, t.cardHi);
        s.Cover({card.left + 10, card.top + 10, card.left + 52, card.bottom - 10});
        s.Line(card.left + 62, card.top + 16, 80, t.text.Alpha(0.7f));
        s.Line(card.left + 62, card.top + 28, 56, dim);
        for (int i = 0; i < 3; ++i) c.Dot({card.left + 70 + i * 16.0f, card.bottom - 16}, 3, i == 1 ? t.accent : dim);
        break;
    }
    case kPillMusic: { // the island pill, with what's playing
        D2D1_RECT_F p = s.Pill(176, 34, kNight);
        s.Cover({p.left + 8, p.top + 7, p.left + 28, p.top + 27});
        s.Words(L"Afterglow", {p.left + 36, p.top, p.right - 40, p.bottom}, 11.5f, kInk);
        s.Bars(p.right - 34, p.bottom - 10, 14, t.accent);
        break;
    }
    case kAlbumArt: { // the cover on the media card
        D2D1_RECT_F card = {in.left + 8, in.top + 6, in.right - 8, in.bottom - 6};
        c.Fill(card, 12, t.cardHi);
        s.Cover({card.left + 10, card.top + 10, card.left + 10 + (card.bottom - card.top - 20), card.bottom - 10});
        s.Line(card.left + 92, card.top + 22, 80, t.text.Alpha(0.7f));
        s.Line(card.left + 92, card.top + 34, 60, dim);
        break;
    }
    case kArtBg: { // the page sits on its cover's colours
        D2D1_RECT_F card = {in.left + 4, in.top + 4, in.right - 4, in.bottom - 4};
        c.FillH(card, 12, kCoverA.Alpha(0.75f), kCoverB.Alpha(0.85f));
        c.Fill(card, 12, Color{0, 0, 0, 0.25f});
        s.Cover({card.left + 12, card.top + 14, card.left + 66, card.top + 68});
        s.Line(card.left + 78, card.top + 26, 80, kInk.Alpha(0.85f));
        s.Line(card.left + 78, card.top + 38, 56, kInk.Alpha(0.5f));
        break;
    }
    case kVisualizer: { // light swelling up from below with the music
        D2D1_RECT_F card = {in.left + 4, in.top + 4, in.right - 4, in.bottom - 4};
        c.Fill(card, 12, t.cardHi);
        c.PushClip(card);
        for (int i = 0; i < 14; ++i) {
            float k = 0.3f + 0.7f * (0.5f + 0.5f * (float)std::sin(s.now * (3.2 + (i % 5) * 0.9) + i * 0.8));
            float x = card.left + 8 + i * ((card.right - card.left - 16) / 14);
            c.FillV({x, card.bottom - 50 * k, x + 9, card.bottom}, 4, t.accent.Alpha(0), t.accent.Alpha(0.75f));
        }
        c.PopClip();
        break;
    }
    case kLyrics: { // the song's words beside it, the sung line lit
        s.Cover({in.left + 6, in.top + 14, in.left + 66, in.top + 74});
        s.Words(L"before it, softly", {in.left + 76, in.top + 12, in.right, in.top + 30}, 11, dim);
        s.Words(L"every word, in time", {in.left + 76, in.top + 32, in.right, in.top + 52}, 12.5f, t.text, Align::Left, Weight::Semi);
        s.Words(L"the next line waits", {in.left + 76, in.top + 54, in.right, in.top + 72}, 11, dim);
        break;
    }
    case kPillAlways: { // the clock between two widgets, always up
        s.Edge(in.top + 2);
        D2D1_RECT_F p = s.Pill(186, 32, kNight, in.top + 12);
        c.Icon(glyph::ClearDay, {p.left + 18, (p.top + p.bottom) / 2}, 14, kInk);
        s.Words(L"21°", {p.left + 28, p.top, p.left + 70, p.bottom}, 11, kInk);
        s.Words(L"10:42", p, 12, kInk, Align::Center, Weight::Semi);
        s.Words(L"84%", {p.right - 52, p.top, p.right - 12, p.bottom}, 11, kInk, Align::Right);
        break;
    }
    case kTimerBubble: { // a running timer, in a bubble beside the pill
        s.Edge(in.top + 2);
        D2D1_RECT_F p = {cx - 96, in.top + 12, cx + 40, in.top + 44};
        c.Fill(p, 16, kNight);
        s.Words(L"Afterglow", {p.left + 14, p.top, p.right, p.bottom}, 11, kInk);
        D2D1_POINT_2F b = {p.right + 26, (p.top + p.bottom) / 2};
        c.Dot(b, 18, kNight);
        c.Arc(b, 15, -1.57f, 4.2f, t.accent, 2.5f);
        s.Words(L"4:59", {b.x - 18, b.y - 8, b.x + 18, b.y + 8}, 10, kInk, Align::Center);
        break;
    }
    case kPinned: { // it stays open, even when the mouse leaves
        s.Edge(in.top + 2);
        c.Fill({in.left + 24, in.top + 2, in.right - 24, in.bottom - 14}, 14, t.cardHi);
        c.Fill({in.left + 24, in.top + 2, in.right - 24, in.top + 16}, 0, t.cardHi);
        s.Words(L"Stays open", {in.left, in.top + 30, in.right, in.top + 50}, 12, t.text, Align::Center);
        c.Icon(glyph::Lock, {cx, in.top + 64}, 16, t.accentHi);
        break;
    }
    case kFloating: { // a card off the edge, with air around it
        s.Edge(in.top + 2);
        D2D1_RECT_F card = {in.left + 30, in.top + 18, in.right - 30, in.bottom - 8};
        c.Fill({card.left + 2, card.top + 6, card.right + 2, card.bottom + 6}, 14, Color{0, 0, 0, 0.18f});
        c.Fill(card, 14, t.cardHi);
        c.Line({cx, in.top + 4}, {cx, card.top - 2}, t.accent.Alpha(0.7f), 1);
        s.Line(card.left + 14, card.top + 16, 70, dim);
        s.Line(card.left + 14, card.top + 28, 110, faint);
        break;
    }
    case kPillDesk: { // up on the desktop, when no window is under it
        D2D1_RECT_F screen = {in.left + 10, in.top + 4, in.right - 10, in.bottom - 4};
        c.FillV(screen, 8, kCoverB.Alpha(0.35f), kCoverA.Alpha(0.25f));
        D2D1_RECT_F p = {cx - 70, screen.top + 6, cx + 70, screen.top + 30};
        c.Fill(p, 12, kNight);
        s.Words(L"10:42", p, 11, kInk, Align::Center, Weight::Semi);
        s.Words(L"The desktop", {screen.left, screen.bottom - 22, screen.right, screen.bottom - 4}, 10.5f, t.text.Alpha(0.6f), Align::Center);
        break;
    }
    case kAppsTheme: { // one look on the desktop, another over your apps
        D2D1_RECT_F a = {in.left + 6, in.top + 26, cx - 6, in.top + 58}, b = {cx + 6, in.top + 26, in.right - 6, in.top + 58};
        c.Fill(a, 16, Color{0.94f, 0.94f, 0.96f, 1});
        c.Fill(b, 16, kNight);
        s.Words(L"10:42", a, 11, Color{0.08f, 0.08f, 0.1f, 1}, Align::Center, Weight::Semi);
        s.Words(L"10:42", b, 11, kInk, Align::Center, Weight::Semi);
        s.Words(L"Desktop", {a.left, a.bottom + 6, a.right, a.bottom + 22}, 10.5f, t.text.Alpha(0.55f), Align::Center);
        s.Words(L"Over apps", {b.left, b.bottom + 6, b.right, b.bottom + 22}, 10.5f, t.text.Alpha(0.55f), Align::Center);
        break;
    }
    case kKeyOnly: { // only the shortcut opens it
        const wchar_t* keys[] = {L"Win", L"Alt", L"A"};
        float w[] = {54, 50, 36}, x = cx - (54 + 50 + 36 + 16) / 2.0f;
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F k = {x, cy - 18, x + w[i], cy + 18};
            c.Fill({k.left, k.top + 3, k.right, k.bottom + 3}, 8, Color{0, 0, 0, 0.25f});
            c.Fill(k, 8, t.cardHi);
            c.Stroke(k, 8, t.hairTop, 1);
            s.Words(keys[i], k, 12, t.text, Align::Center);
            x += w[i] + 8;
        }
        break;
    }
    case kPillFull: { // the pill stays over a fullscreen app
        D2D1_RECT_F screen = {in.left + 10, in.top + 4, in.right - 10, in.bottom - 4};
        c.Fill(screen, 8, Color{0.02f, 0.02f, 0.03f, 1});
        c.Icon(glyph::Play, {cx, cy + 10}, 22, kInk.Alpha(0.5f), true);
        D2D1_RECT_F p = {cx - 64, screen.top + 6, cx + 64, screen.top + 28};
        c.Fill(p, 11, Color{0.16f, 0.16f, 0.2f, 1});
        s.Words(L"Afterglow", {p.left + 12, p.top, p.right, p.bottom}, 10.5f, kInk);
        s.Bars(p.right - 26, p.bottom - 7, 10, t.accent);
        break;
    }
    case kPillFullOpen: { // a click on that pill opens the panel
        D2D1_RECT_F p = s.Pill(150, 28, kNight, in.top + 8);
        s.Words(L"Afterglow", {p.left + 14, p.top, p.right, p.bottom}, 11, kInk);
        c.Fill({in.left + 34, p.bottom + 8, in.right - 34, in.bottom - 4}, 12, t.cardHi.Alpha(0.6f));
        D2D1_POINT_2F m = {cx + 30, p.top + 18}; // a pointer, clicking
        c.Line(m, {m.x, m.y + 16}, t.text, 2);
        c.Line(m, {m.x + 11, m.y + 11}, t.text, 2);
        c.Line({m.x, m.y + 16}, {m.x + 11, m.y + 11}, t.text, 2);
        break;
    }
    case kPowerPills: { // plugging in or unplugging drops a pill
        D2D1_RECT_F p = s.Pill(170, 34, kNight);
        c.Icon(glyph::Bolt, {p.left + 20, cy}, 16, Color{0.45f, 0.9f, 0.55f, 1}, true);
        s.Words(L"Charging  ·  64%", {p.left + 34, p.top, p.right - 8, p.bottom}, 11.5f, kInk);
        break;
    }
    case kHud: { // the volume keys show astraeus' own pill
        D2D1_RECT_F p = s.Pill(180, 34, kNight);
        c.Icon(glyph::VolumeUp, {p.left + 20, cy}, 16, kInk, true);
        c.Fill({p.left + 38, cy - 3, p.right - 46, cy + 3}, 3, kInk.Alpha(0.25f));
        c.Fill({p.left + 38, cy - 3, p.left + 38 + (p.right - p.left - 84) * 0.7f, cy + 3}, 3, kInk);
        s.Words(L"70", {p.right - 40, p.top, p.right - 12, p.bottom}, 11, kInk, Align::Right);
        break;
    }
    case kShelfCatch: case kShelfKeep: case kShelfWipe: { // the shelf's cards
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F card = {in.left + 14 + i * 64.0f, in.top + 18, in.left + 72 + i * 64.0f, in.bottom - 18};
            float a = kind == kShelfWipe && i > 0 ? 0.35f : 1.0f; // clearing: the unpinned ones go
            c.Fill(card, 10, t.cardHi.Alpha(a));
            s.Line(card.left + 8, card.top + 12, 34, t.text.Alpha(0.6f * a));
            s.Line(card.left + 8, card.top + 22, 22, t.text.Alpha(0.3f * a));
        }
        if (kind == kShelfCatch) s.Words(L"Copied", {in.left, in.bottom - 16, in.right, in.bottom}, 10.5f, t.accentHi, Align::Center);
        if (kind == kShelfKeep) c.Icon(glyph::Lock, {in.right - 18, in.top + 14}, 15, t.accentHi);
        if (kind == kShelfWipe) s.Words(L"Pinned stays", {in.left, in.bottom - 16, in.right, in.bottom}, 10.5f, t.accentHi, Align::Center);
        break;
    }
    case kStageLyrBtn: case kStageEditBtn: { // the fullscreen player's corner marks
        D2D1_RECT_F screen = {in.left + 10, in.top + 4, in.right - 10, in.bottom - 4};
        c.FillH(screen, 8, kCoverB.Alpha(0.5f), kCoverA.Alpha(0.4f));
        s.Cover({screen.left + 14, screen.top + 18, screen.left + 60, screen.top + 64});
        s.Line(screen.left + 72, screen.top + 30, 70, kInk.Alpha(0.85f));
        s.Line(screen.left + 72, screen.top + 42, 50, kInk.Alpha(0.4f));
        D2D1_POINT_2F o = {screen.right - 16, screen.top + 14};
        c.Dot(o, 11, Color{1, 1, 1, 0.18f});
        c.Icon(kind == kStageEditBtn ? glyph::Edit : glyph::Lyrics, o, 12, kInk);
        break;
    }
    case kTourNext: { // the first-run tour
        D2D1_RECT_F card = {in.left + 20, in.top + 10, in.right - 20, in.bottom - 10};
        c.Fill(card, 12, t.cardHi);
        s.Words(L"Welcome to Astraeus", {card.left + 12, card.top + 8, card.right, card.top + 26}, 12, t.text, Align::Left, Weight::Semi);
        s.Line(card.left + 12, card.top + 34, 120, dim);
        for (int i = 0; i < 5; ++i) c.Dot({card.left + 16 + i * 10.0f, card.bottom - 14}, 2.5f, i == 0 ? t.accent : faint);
        s.Words(L"Next", {card.right - 50, card.bottom - 24, card.right - 10, card.bottom - 4}, 11, t.accentHi, Align::Right);
        break;
    }
    case kA11yFollow: { // the text grows with windows' own setting
        s.Words(L"Aa", {in.left + 24, cy - 14, cx - 10, cy + 14}, 14, dim, Align::Center);
        s.Words(L"→", {cx - 12, cy - 12, cx + 12, cy + 12}, 14, dim, Align::Center);
        s.Words(L"Aa", {cx + 10, cy - 22, in.right - 24, cy + 22}, 26, t.text, Align::Center);
        break;
    }
    case kA11yKeys: { // a focus ring walks the controls
        for (int i = 0; i < 4; ++i) {
            D2D1_RECT_F k = {in.left + 14 + i * 52.0f, cy - 18, in.left + 56 + i * 52.0f, cy + 18};
            c.Fill(k, 10, t.cardHi);
            if (i == 1) c.Stroke({k.left - 3, k.top - 3, k.right + 3, k.bottom + 3}, 12, t.accent, 2);
        }
        s.Words(L"Tab", {in.left, in.bottom - 18, in.right, in.bottom}, 10.5f, dim, Align::Center);
        break;
    }
    case kA11yReader: { // a screen reader names what's there
        c.Icon(glyph::VolumeUp, {in.left + 26, cy}, 20, t.accentHi, true);
        s.Words(L"“Home, button”", {in.left + 46, cy - 12, in.right, cy + 12}, 13, t.text);
        break;
    }
    default:
        break;
    }
}

}

void Want(Ui& ui, int kind, bool hover) {
    if (!hover || kind == kNone) return;
    seen = true;
    if (kind != cur) cur = kind, since = ui.Now();
}

int ForId(const char* id) {
    static const struct { const char* id; int kind; } kMap[] = {
        {"set.pwr.pills", kPowerPills}, {"set.pill.always", kPillAlways}, {"set.pill.bubble", kTimerBubble}, {"set.pinned", kPinned},
        {"set.floating", kFloating}, {"set.tour.next", kTourNext}, {"set.stage.lyrbtn", kStageLyrBtn}, {"set.stage.editbtn", kStageEditBtn},
        {"set.keyonly", kKeyOnly}, {"set.pill.full", kPillFull}, {"set.pill.fullopen", kPillFullOpen}, {"set.shelf.cap", kShelfCatch},
        {"set.shelf.keep", kShelfKeep}, {"set.shelf.wipe", kShelfWipe}, {"set.a11y.follow", kA11yFollow}, {"set.a11y.keys", kA11yKeys},
        {"set.a11y.reader", kA11yReader},
    };
    for (const auto& e : kMap)
        if (!std::strcmp(e.id, id)) return e.kind;
    return kNone;
}

void Draw(Ui& ui, const Theme& t, const D2D1_RECT_F& bounds) {
    if (!seen) cur = kNone; // nothing under the mouse said so: it goes
    seen = false;
    bool want = cur != kNone && ui.Now() - since >= kDelay;
    if (want) shown = cur;
    if (cur != kNone && !want) ui.KeepAlive(); // waiting out the delay: frames keep coming so it can appear
    float a = ui.Smooth(Ui::Id("preview.fade"), want ? 1.0f : 0, kFade);
    if (a < 0.01f || shown == kNone) return;
    Canvas& c = ui.C();
    D2D1_POINT_2F m = ui.In().mouse;
    float x = m.x + kOff, y = m.y + kOff; // below and right of the pointer; turned over where it would leave the sheet
    if (x + kW > bounds.right - 6) x = m.x - kOff - kW;
    if (y + kH > bounds.bottom - 6) y = m.y - kOff - kH;
    x = (std::max)(x, bounds.left + 6), y = (std::max)(y, bounds.top + 6);
    float lift = (1 - a) * 6;
    D2D1_RECT_F r = {x, y + lift, x + kW, y + lift + kH};
    float rad = kH / 2.4f;
    c.Fill({r.left, r.top + 8, r.right, r.bottom + 10}, rad, Color{0, 0, 0, 0.22f * a});
    c.Fill(r, rad, t.card.Alpha(0.88f * a)); // see-through, so the settings under it still show
    c.Stroke(r, rad, t.hairTop.Alpha(t.hairTop.a * a), 1);
    if (a >= kSketchAt) {
        c.PushClip(r);
        Sketch s{c, t, {r.left + kPad + 6, r.top + kPad, r.right - kPad - 6, r.bottom - kPad}, ui.Now()};
        Paint(s, shown);
        c.PopClip();
    }
    ui.KeepAlive(); // the little bars move
}

}
