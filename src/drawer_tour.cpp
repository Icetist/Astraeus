// The tour: a few steps over the panel, a soft light on each place that can be made your own, and a card saying how.
// It plays the first time the panel opens after a launch while its switch is on, and Settings can play it any time.
#include <algorithm>
#include <cmath>
#include <vector>

#include "drawer.h"
#include "page_sheet.h"

namespace {

enum Spot { kSpotNone = -1, kSpotRail, kSpotTiles, kSpotCover, kSpotAdd, kSpotSettings };
struct Step {
    const wchar_t* title;
    const wchar_t* body;
    int page; // the page it shows, -1 whichever is up; a page not on the rail leaves the step out
    int spot;
    const wchar_t* other; // said instead when there's nothing to point at: no song playing, the tile taken off
};
const Step kSteps[] = {
    {L"Welcome to Astraeus", L"A quick look at how to make it yours.\nIt takes about a minute.", -1, kSpotNone, nullptr},
    {L"Your pages", L"Click an icon to switch pages. Not\nevery page is here at first: right-click\nthe rail to add more, like Widgets,\nreorder them, or pick where it opens.", -1, kSpotRail, nullptr},
    {L"Quick tiles", L"Right-click the tiles to choose which\nones show, and drag them into order.", Model::kPgTools, kSpotTiles, nullptr},
    {L"Fullscreen player", L"Click the album cover for fullscreen.\nThe pencil in its top corner lets you\nmove, resize and restyle all of it.", Model::kPgMedia, kSpotCover,
     L"When something's playing, click the\nalbum cover for fullscreen. The pencil\nin its top corner lets you move, resize\nand restyle all of it."},
    {L"Widgets", L"Add widgets puts clocks, stocks,\nweather and more on this board.", Model::kPgWidgets, kSpotAdd, nullptr},
    {L"Settings", L"This tile opens Settings, where the\nrest of Astraeus can be made yours.", Model::kPgTools, kSpotSettings,
     L"Settings has the rest of Astraeus.\nOpen it from the tray icon's menu."},
    {L"You're all set", L"Take the tour again any time from\nSettings, under Updates.", -1, kSpotNone, nullptr},
};
constexpr int kStepCount = (int)(sizeof(kSteps) / sizeof(kSteps[0]));

constexpr float kDim = 0.55f, kSpotPad = 6, kSpotRad = 16, kRing = 2, kPulseGrow = 9, kPulseSec = 1.6f; // the light
constexpr float kCardW = 300, kCardPad = 18, kCardGap = 14, kCardEdge = 12, kCardRad = 18, kLineH = 17, kTitleH = 24, kBtnH = 30, kDotsH = 10, kSwitchH = 30; // the card
constexpr float kNextW = 92, kAgainW = 116, kBackW = 70, kSkipW = 64, kBtnGap = 8, kDotR = 3, kDotOnW = 14, kDotGap = 6, kRise = 8;

int Lines(const wchar_t* s) {
    int n = 1;
    for (; *s; ++s) n += *s == L'\n';
    return n;
}

}

bool Drawer::TourShows(int i) const {
    const Step& s = kSteps[i];
    return s.page < 0 || model.HasPage(s.page) || s.spot == kSpotSettings; // settings is always told: without tools, by the tray
}

void Drawer::TourGo(int from, int dir) {
    for (int i = from; i >= 0 && i < kStepCount; i += dir) {
        if (!TourShows(i)) continue;
        tourStep = i;
        ++tourNth;
        if (kSteps[i].page >= 0) SetPage(kSteps[i].page);
        Redraw();
        return;
    }
}

void Drawer::TourStart() {
    model.CloseBt(); // the panel itself, nothing over it
    model.CloseWifi();
    model.CloseSettings();
    model.CloseCal();
    model.CloseMixer();
    tourSeen = true;
    tourAgain = false; // the last card asks again each time
    ++tourRun;
    TourGo(0, 1);
}

void Drawer::TourEnd() {
    tourStep = -1;
    int next = tourAgain ? 1 : 0;
    if (model.tourNext != next) model.SetPref(model.tourNext, L"TourNext", next);
    Redraw();
}

void Drawer::DrawTour(const Theme& t, float panelW, float panelH) {
    if (tourStep < 0) return;
    Canvas& c = *canvas;
    const Step& st = kSteps[tourStep];
    int spot = st.spot;
    D2D1_RECT_F want{};
    if (spot == kSpotRail) want = RailBox();
    else if (spot >= 0) want = tourSpots[spot - kSpotTiles];
    bool lit = spot >= 0 && Width(want) > 1 && Height(want) > 1;
    if (spot == kSpotSettings && page != Model::kPgTools) lit = false; // the page is still sliding in: no old spot
    bool other = st.other && (!lit || (spot == kSpotCover && !model.hasTrack)); // nothing playing still points where the cover goes
    const wchar_t* body = other ? st.other : st.body;
    D2D1_RECT_F full = {0, 0, panelW, panelH};
    float show = ui.SmoothFrom(Ui::Id("tour.show", tourRun), 1, 0, 10); // the whole thing settles in
    float hole = ui.Smooth(Ui::Id("tour.hole"), lit ? 1.0f : 0, 10);
    D2D1_POINT_2F mid = {panelW / 2, panelH / 2};
    if (lit) want = Inset(want, -kSpotPad, -kSpotPad);
    else want = {mid.x, mid.y, mid.x, mid.y}; // nothing to point at: the light closes to the middle
    D2D1_RECT_F r = {ui.Smooth(Ui::Id("tour.l"), want.left, 12), ui.Smooth(Ui::Id("tour.t"), want.top, 12), ui.Smooth(Ui::Id("tour.r"), want.right, 12), ui.Smooth(Ui::Id("tour.b"), want.bottom, 12)}; // it glides from step to step
    float rad = (std::min)({kSpotRad, Width(r) / 2, Height(r) / 2});
    Color dim = Color{0, 0, 0, kDim * show};
    ComPtr<ID2D1RectangleGeometry> outer;
    ComPtr<ID2D1RoundedRectangleGeometry> inner;
    ComPtr<ID2D1PathGeometry> veil;
    ComPtr<ID2D1GeometrySink> sink;
    bool cut = hole > 0.01f && Width(r) > 1 && Height(r) > 1 && SUCCEEDED(gfx::D2D()->CreateRectangleGeometry(full, &outer)) &&
               SUCCEEDED(gfx::D2D()->CreateRoundedRectangleGeometry({r, rad, rad}, &inner)) && SUCCEEDED(gfx::D2D()->CreatePathGeometry(&veil)) && SUCCEEDED(veil->Open(&sink)) &&
               SUCCEEDED(outer->CombineWithGeometry(inner.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get())) && SUCCEEDED(sink->Close());
    if (cut) c.FillGeoV(veil.Get(), full, dim, dim);
    else c.Fill(full, 0, dim);
    if (cut) { // a ring around the light, and a softer one breathing out from it
        float phase = (float)std::fmod(ui.Now(), (double)kPulseSec) / kPulseSec, grow = kPulseGrow * phase;
        c.Stroke(r, rad, t.accentHi.Alpha(0.9f * hole * show), kRing);
        c.Stroke(Inset(r, -grow, -grow), rad + grow, t.accentHi.Alpha(0.45f * (1 - phase) * hole * show), 1.5f);
        ui.KeepAlive();
    }

    std::vector<int> shown; // the steps this pc has, for the dots
    for (int i = 0; i < kStepCount; ++i)
        if (TourShows(i)) shown.push_back(i);
    int at = 0;
    for (size_t i = 0; i < shown.size(); ++i)
        if (shown[i] == tourStep) at = (int)i;
    bool first = at == 0, last = at + 1 == (int)shown.size();
    float cw = (std::min)(kCardW, panelW - 2 * kCardEdge);
    float ch = 2 * kCardPad + kTitleH + Lines(body) * kLineH + kCardGap + kDotsH + kCardGap + (last ? kSwitchH + kBtnGap : 0) + kBtnH;
    float cx, cy;
    if (lit && want.right + kCardGap + cw <= panelW - kCardEdge) cx = want.right + kCardGap, cy = (want.top + want.bottom - ch) / 2; // beside the light, where there's room
    else if (lit && want.left - kCardGap - cw >= kCardEdge) cx = want.left - kCardGap - cw, cy = (want.top + want.bottom - ch) / 2;
    else if (lit && want.bottom + kCardGap + ch <= panelH - kCardEdge) cx = (want.left + want.right - cw) / 2, cy = want.bottom + kCardGap;
    else if (lit && want.top - kCardGap - ch >= kCardEdge) cx = (want.left + want.right - cw) / 2, cy = want.top - kCardGap - ch;
    else cx = (panelW - cw) / 2, cy = (panelH - ch) / 2;
    cx = (std::clamp)(cx, kCardEdge, (std::max)(kCardEdge, panelW - cw - kCardEdge));
    cy = (std::clamp)(cy, kCardEdge, (std::max)(kCardEdge, panelH - ch - kCardEdge));
    cx = ui.Smooth(Ui::Id("tour.cx"), cx, 12), cy = ui.Smooth(Ui::Id("tour.cy"), cy, 12);
    float h = ui.Smooth(Ui::Id("tour.ch"), ch, 14);
    float in = ui.SmoothFrom(Ui::Id("tour.in", tourNth), 1, 0, 14) * show; // each step's words drift up into place
    D2D1_RECT_F card = Rect(cx, cy, cw, h);
    c.Fill(Rect(card.left, card.top + 4, cw, h), kCardRad, Color{0, 0, 0, 0.25f * show});
    Color fill = t.bg;
    fill.a = 0.98f;
    c.Fill(card, kCardRad, fill.Alpha(show));
    c.StrokeV(card, kCardRad, t.hairTop.Alpha(show), t.hairBot.Alpha(show), 1);
    c.PushClip(card);
    bool layer = in < 0.999f;
    if (layer) ctx->PushLayer(D2D1::LayerParameters1(card, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), in), nullptr);
    float x = card.left + kCardPad, y = card.top + kCardPad + (1 - in) * kRise, w = cw - 2 * kCardPad;
    c.Text(st.title, Rect(x, y, w, kTitleH), Weight::Semi, 15, t.text);
    y += kTitleH;
    c.Text(body, Rect(x, y, w, Lines(body) * kLineH), Weight::Regular, 12, t.textDim);
    y += Lines(body) * kLineH + kCardGap;
    float dx = x;
    for (size_t i = 0; i < shown.size(); ++i) { // where it is: the current one a short pill
        float on = ui.Smooth(Ui::Id("tour.dot", (int)i), (int)i == at ? 1.0f : 0, 14), dw = 2 * kDotR + (kDotOnW - 2 * kDotR) * on;
        c.Fill(Rect(dx, y + kDotsH / 2 - kDotR, dw, 2 * kDotR), kDotR, t.textLow.Mix(t.accent, on));
        dx += dw + kDotGap;
    }
    y += kDotsH + kCardGap;
    bool live = in > 0.5f;
    int go = 0; // -1 back, 1 next, 2 done, 3 again, 4 skip to the last card
    if (last) { // the switch for next time, then done or once more
        Ui::Hit words = live ? ui.Button(Ui::Id("tour.again.row"), Rect(x, y, w - 52, kSwitchH)) : Ui::Hit{}; // the words flip it too
        if (live) ui.NoStop();
        c.Text(L"Show the tour next time", Rect(x, y, w - 56, kSwitchH), Weight::Regular, 12, t.text);
        if ((w::Switch(ui, t, Ui::Id("tour.again"), {x + w - 24, y + kSwitchH / 2}, tourAgain, live) || words.clicked) && live) tourAgain = !tourAgain;
        if (live) ui.Name(L"Show the tour next time");
        y += kSwitchH + kBtnGap;
        if (sheet::Act(ui, t, Ui::Id("tour.done"), Rect(card.right - kCardPad - kNextW, y, kNextW, kBtnH), L"Got it", true, false, live)) go = 2;
        if (sheet::Act(ui, t, Ui::Id("tour.redo"), Rect(card.right - kCardPad - kNextW - kBtnGap - kAgainW, y, kAgainW, kBtnH), L"Take it again", false, false, live)) go = 3;
    } else {
        float bx = card.right - kCardPad - kNextW;
        if (sheet::Act(ui, t, Ui::Id("tour.next"), Rect(bx, y, kNextW, kBtnH), first ? L"Let's go" : L"Next", true, false, live)) go = 1;
        if (!first && sheet::Act(ui, t, Ui::Id("tour.back"), Rect(bx - kBtnGap - kBackW, y, kBackW, kBtnH), L"Back", false, false, live)) go = -1;
        if (sheet::Act(ui, t, Ui::Id("tour.skip"), Rect(x, y, kSkipW, kBtnH), L"Skip", false, false, live)) go = 4;
    }
    if (layer) ctx->PopLayer();
    c.PopClip();
    ui.Button(Ui::Id("tour.veil"), full); // last: the card's buttons take their presses first; nothing under it can be pressed
    ui.NoStop();
    if (go == 1) TourGo(tourStep + 1, 1);
    else if (go == -1) TourGo(tourStep - 1, -1);
    else if (go == 2) TourEnd();
    else if (go == 3) TourGo(0, 1), ++tourRun;
    else if (go == 4) TourGo(kStepCount - 1, -1); // its switch and its buttons still have the say
}
