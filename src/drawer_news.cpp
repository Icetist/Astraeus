// The update's own screen. The first time the panel opens after an update it fills the whole panel, the rail too, in the
// look's own colour: what came and what it's called, then the notes or the way on. The notes read in the same place,
// and Settings opens them here as well. They're read from the exe when it shows and let go once it's gone.
#include <algorithm>
#include <cmath>

#include "drawer.h"
#include "glyphs.h"
#include "page_sheet.h"
#include "sys.h"
#include "version.h"

namespace {

constexpr float kRise = 16, kStagger = 0.07f, kRiseSec = 0.6f; // each part drifts up into place, a beat after the one above
constexpr int kStaggerMost = 10; // parts further down rise with the tenth
constexpr float kTint = 0.16f, kGlowA = 0.22f, kGlassBody = 0.88f; // the accent in the ground at its top; the light behind; glass keeps a breath of its blur
constexpr float kEdge = 28; // from the panel's sides
constexpr float kHelloW = 460, kBadgeR = 36, kBadgeIcon = 34, kRingGap = 9, kBadgeAfter = 22;
constexpr float kEyebrowH = 16, kEyebrowSize = 11, kEyebrowSp = 1.6f, kTitleH = 38, kTitleSize = 27, kNameH = 24, kNameSize = 16;
constexpr float kIntroSize = 13, kIntroGap = 16, kIntroMin = 40; // a line of the notes this long says something
constexpr float kChipH = 26, kChipPad = 12, kChipGap = 8, kChipSize = 11.5f, kChipsGap = 20;
constexpr int kChipsMost = 4;
constexpr float kBtnW = 156, kBtnH = 38, kBtnGap = 10, kBtnsGap = 30;
constexpr float kNotesW = 620, kHeadTitleH = 26, kHeadSubH = 18, kHeadBelow = 18, kHeadBadgeR = 17, kGoW = 116, kBackW = 84, kHeadBtnH = 34, kWheel = 60;
constexpr float kItemSize = 12.5f, kParaSize = 13, kAsideSize = 12, kGroupSize = 15, kHeadSize = 11, kHeadSp = 1.4f;
constexpr float kLeading = 1.5f, kBaseline = 1.18f; // the lines of a note this far apart: easy reading
constexpr float kDotX = 5, kDotR = 2.5f, kItemIndent = 18, kAsidePad = 14, kAsideBar = 3, kAsideR = 12;
constexpr float kHeadFirst = 8, kHeadGap = 30, kHeadRule = 22, kHeadAfter = 16, kGroupGap = 16, kGroupH = 24, kGroupAfter = 4;
constexpr float kItemAfter = 9, kParaAfter = 14, kAsideGap = 8, kAsideAfter = 12, kTopPad = 6, kViewSlack = 40;

using Note = Model::Note;

ComPtr<IDWriteTextLayout> Wrap(const std::wstring& marked, float w, float size, bool centre) { // **bold** set heavier, the marks gone
    std::wstring s;
    std::vector<DWRITE_TEXT_RANGE> bold;
    bool in = false;
    UINT32 from = 0;
    for (size_t i = 0; i < marked.size(); ++i) {
        if (marked[i] == L'*' && i + 1 < marked.size() && marked[i + 1] == L'*') {
            if (in) bold.push_back({from, (UINT32)s.size() - from});
            else from = (UINT32)s.size();
            in = !in, ++i;
            continue;
        }
        if (marked[i] != L'`') s += marked[i];
    }
    constexpr float kTall = 1e5f;
    ComPtr<IDWriteTextLayout> l;
    IDWriteTextFormat* f = gfx::TextFmt(Weight::Regular, size, centre ? Align::Center : Align::Left);
    if (s.empty() || !f || FAILED(gfx::DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, kTall, &l))) return nullptr;
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); // the shared format is one line; this layout alone wraps
    l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * kLeading, size * kBaseline);
    for (const DWRITE_TEXT_RANGE& r : bold) l->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
    return l;
}

float LayoutH(IDWriteTextLayout* l) {
    DWRITE_TEXT_METRICS tm{};
    return l && SUCCEEDED(l->GetMetrics(&tm)) ? tm.height : 0;
}

void Draw(Canvas& c, IDWriteTextLayout* l, float x, float y, Color col) {
    if (!l || col.a <= 0.002f) return;
    ComPtr<ID2D1SolidColorBrush> ink; // made on this frame's target: one kept across a rebuilt device would fail the frame
    if (SUCCEEDED(c.Target()->CreateSolidColorBrush(col.D2D(), &ink))) c.Target()->DrawTextLayout({x, y}, l, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

std::wstring Upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

float Body(const Theme& t) { return t.glass ? (std::max)(t.bg.a, kGlassBody) : 1.0f; }

Color Ground(const Theme& t, float tint) {
    Color g = t.bg.Mix(t.accent, tint);
    g.a = Body(t);
    return g;
}

}

void Drawer::NewsShow(int view) {
    bool have = model.LoadNotes(); // the hello's name and words come from them too
    if (view == 2 && !have) { // none built in: the page online has them
        sys::OpenUri(L"https://github.com/Icetist/Astraeus/releases/tag/v" APP_VER_W);
        return;
    }
    newsFrom = view == 2 ? newsView : 0;
    newsView = newsLast = view;
    newsAt = -1;
    newsScroll = 0;
    Redraw();
}

void Drawer::NewsEnd() {
    if (model.newsDue) model.DropNews();
    newsView = 0; // newsLast stays while it fades; the notes go after
    Redraw();
}

float Drawer::NewsPart(int i) const {
    if (model.reduceMotion) return 1;
    if (newsAt < 0) return 0;
    return anim::EaseOut(anim::Clamp01((float)(ui.Now() - newsAt - (std::min)(i, kStaggerMost) * kStagger) / kRiseSec));
}

void Drawer::DrawNews(const Theme& t, float panelW, float panelH, float show) {
    Canvas& c = *canvas;
    if (newsAt < 0) newsAt = ui.Now();
    if (ui.Now() - newsAt < kRiseSec + kStaggerMost * kStagger) ui.KeepAlive(); // while its parts rise, then still
    bool layer = show < 0.999f;
    if (layer) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), show), nullptr);
    D2D1_RECT_F full = {0, 0, panelW, panelH};
    D2D1_RECT_F bleed = edgeNow == 0 ? D2D1_RECT_F{0, -cfg.corner, panelW, panelH} : edgeNow == 1 ? D2D1_RECT_F{-cfg.corner, 0, panelW, panelH}
                                                                                     : D2D1_RECT_F{0, 0, panelW + cfg.corner, panelH}; // as the cover's backdrop: square on the docked side
    c.FillV(bleed, cfg.corner, Ground(t, kTint), Ground(t, 0));
    c.Glow({panelW / 2, panelH * 0.3f}, panelW * 0.6f, panelH * 0.45f, t.accent.Alpha(newsLast == 1 ? kGlowA : kGlowA / 2)); // a soft light the words sit in
    if (newsLast == 2) NewsNotes(t, panelW, panelH);
    else NewsHello(t, panelW, panelH);
    if (layer) ctx->PopLayer();
    if (newsView) { // last: its own buttons take their presses first; nothing under it can be pressed
        ui.Button(Ui::Id("news.veil"), full);
        ui.NoStop();
    }
}

void Drawer::NewsHello(const Theme& t, float panelW, float panelH) {
    Canvas& c = *canvas;
    float cw = (std::min)(kHelloW, panelW - 2 * kEdge), cx = panelW / 2, left = cx - cw / 2;
    float rise = model.reduceMotion ? 0 : kRise;
    const Note* intro = nullptr; // the first words long enough to say something
    for (const Note& n : model.notes)
        if (n.kind == Note::kPara && n.text.size() > kIntroMin) {
            intro = &n;
            break;
        }
    if (intro && (!newsIntro || std::fabs(newsIntroW - cw) > 0.5f)) newsIntro = Wrap(intro->text, cw, kIntroSize, true), newsIntroW = cw;
    std::vector<std::wstring> chips; // what it brought: its groups, or else its sections
    for (int pass = 0; pass < 2 && chips.empty(); ++pass)
        for (const Note& n : model.notes)
            if (n.kind == (pass ? Note::kHead : Note::kGroup) && (int)chips.size() < kChipsMost) chips.push_back(n.text);
    std::vector<float> widths;
    float chipsW = 0;
    for (const std::wstring& s : chips) {
        float w = c.Measure(s, Weight::Medium, kChipSize) + 2 * kChipPad, gap = widths.empty() ? 0 : kChipGap;
        if (chipsW + gap + w > cw) break; // only what fits on one line
        chipsW += gap + w;
        widths.push_back(w);
    }
    float introH = intro ? LayoutH(newsIntro.Get()) : 0;
    auto total = [&] {
        return 2 * (kBadgeR + kRingGap) + kBadgeAfter + kEyebrowH + kTitleH + (model.notesName.empty() ? 0 : kNameH) + (introH > 0 ? kIntroGap + introH : 0) +
               (widths.empty() ? 0 : kChipsGap + kChipH) + kBtnsGap + kBtnH;
    };
    if (total() > panelH - 2 * kEdge) introH = 0; // a short panel: the words give way first, then the chips
    if (total() > panelH - 2 * kEdge) widths.clear();
    float y = (std::max)(kEdge, (panelH - total()) / 2);

    float p = NewsPart(0), oy = (1 - p) * rise; // a badge, ringed, with the spark in it
    D2D1_POINT_2F mid = {cx, y + kRingGap + kBadgeR + oy};
    c.Dot(mid, kBadgeR + kRingGap, t.accent.Alpha(0.10f * p));
    c.Dot(mid, kBadgeR, t.accent.Alpha(0.22f * p));
    c.Stroke(Rect(mid.x - kBadgeR, mid.y - kBadgeR, 2 * kBadgeR, 2 * kBadgeR), kBadgeR, t.accentHi.Alpha(0.55f * p), 1.5f);
    c.Icon(glyph::Sparkle, mid, kBadgeIcon, t.accentHi.Alpha(p), true);
    y += 2 * (kBadgeR + kRingGap) + kBadgeAfter;

    p = NewsPart(1), oy = (1 - p) * rise;
    c.TextSp(L"VERSION " APP_VER_W, Rect(left, y + oy, cw, kEyebrowH), Weight::Semi, kEyebrowSize, t.accentHi.Alpha(p), kEyebrowSp, Align::Center);
    y += kEyebrowH;
    p = NewsPart(2), oy = (1 - p) * rise;
    c.Text(L"Astraeus has updated", Rect(left, y + oy, cw, kTitleH), Weight::Semi, kTitleSize, t.text.Alpha(p), Align::Center);
    y += kTitleH;
    if (!model.notesName.empty()) {
        c.Text(model.notesName, Rect(left, y + oy, cw, kNameH), Weight::Regular, kNameSize, t.textDim.Alpha(p), Align::Center);
        y += kNameH;
    }
    if (introH > 0) {
        p = NewsPart(3), oy = (1 - p) * rise;
        y += kIntroGap;
        Draw(c, newsIntro.Get(), left, y + oy, t.textDim.Alpha(p));
        y += introH;
    }
    if (!widths.empty()) {
        p = NewsPart(4), oy = (1 - p) * rise;
        y += kChipsGap;
        float x = cx - chipsW / 2;
        for (size_t i = 0; i < widths.size(); ++i) {
            D2D1_RECT_F r = Rect(x, y + oy, widths[i], kChipH);
            c.Fill(r, kChipH / 2, t.accent.Alpha(0.14f * p));
            c.Stroke(r, kChipH / 2, t.accent.Alpha(0.32f * p), 1);
            c.Text(chips[i], r, Weight::Medium, kChipSize, t.accentHi.Alpha(p), Align::Center);
            x += widths[i] + kChipGap;
        }
        y += kChipH;
    }
    p = NewsPart(5), oy = (1 - p) * rise;
    y += kBtnsGap;
    bool live = newsView == 1 && p > 0.5f, fade = p < 0.999f;
    float bx = cx - (2 * kBtnW + kBtnGap) / 2;
    if (fade) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), p), nullptr);
    bool notes = sheet::Act(ui, t, Ui::Id("news.notes"), Rect(bx, y + oy, kBtnW, kBtnH), L"Release notes", false, false, live);
    bool go = sheet::Act(ui, t, Ui::Id("news.go"), Rect(bx + kBtnW + kBtnGap, y + oy, kBtnW, kBtnH), L"Continue", true, false, live);
    if (fade) ctx->PopLayer();
    if (notes) NewsShow(2);
    else if (go) NewsEnd();
}

void Drawer::NewsNotes(const Theme& t, float panelW, float panelH) {
    Canvas& c = *canvas;
    float rise = model.reduceMotion ? 0 : kRise;
    float colW = (std::min)(kNotesW, panelW - 2 * kEdge), colX = (panelW - colW) / 2;

    float p = NewsPart(0), y = kEdge + (1 - p) * rise / 2; // the head: what this is, and the way on
    bool live = newsView == 2 && p > 0.5f, fade = p < 0.999f;
    if (fade) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), p), nullptr);
    float by = y + (kHeadTitleH + kHeadSubH - kHeadBtnH) / 2;
    D2D1_RECT_F goR = Rect(colX + colW - kGoW, by, kGoW, kHeadBtnH);
    bool go = sheet::Act(ui, t, Ui::Id("news.n.go"), goR, model.newsDue ? L"Continue" : L"Done", true, false, live);
    bool back = false;
    float textR = goR.left - 12;
    if (newsFrom == 1) { // came from the hello: back to it
        D2D1_RECT_F backR = Rect(goR.left - kBtnGap - kBackW, by, kBackW, kHeadBtnH);
        back = sheet::Act(ui, t, Ui::Id("news.n.back"), backR, L"Back", false, false, live);
        textR = backR.left - 12;
    }
    D2D1_POINT_2F badge = {colX + kHeadBadgeR, y + (kHeadTitleH + kHeadSubH) / 2};
    c.Dot(badge, kHeadBadgeR, t.accent.Alpha(0.2f));
    c.Icon(glyph::Sparkle, badge, kHeadBadgeR + 1, t.accentHi, true);
    float tx = colX + 2 * kHeadBadgeR + 12;
    c.Text(L"What's new", Rect(tx, y, textR - tx, kHeadTitleH), Weight::Semi, 18, t.text);
    std::wstring sub = L"Version " APP_VER_W;
    if (!model.notesName.empty()) sub += L"  ·  " + model.notesName;
    c.Text(sub, Rect(tx, y + kHeadTitleH, textR - tx, kHeadSubH), Weight::Regular, 12, t.textDim);
    if (fade) ctx->PopLayer();
    float top = kEdge + kHeadTitleH + kHeadSubH + kHeadBelow;
    c.Line({colX, top}, {colX + colW, top}, t.line.Alpha(p), 1, false);

    if (std::fabs(newsWidth - colW) > 0.5f || newsLays.size() != model.notes.size()) { // laid out again only when the width changes
        newsLays.assign(model.notes.size(), nullptr);
        newsHeights.assign(model.notes.size(), 0);
        for (size_t i = 0; i < model.notes.size(); ++i) {
            const Note& n = model.notes[i];
            if (n.kind == Note::kItem) newsLays[i] = Wrap(n.text, colW - kItemIndent, kItemSize, false);
            else if (n.kind == Note::kPara) newsLays[i] = Wrap(n.text, colW, kParaSize, false);
            else if (n.kind == Note::kAside) newsLays[i] = Wrap(n.text, colW - 2 * kAsidePad - kAsideBar, kAsideSize, false);
            newsHeights[i] = LayoutH(newsLays[i].Get());
        }
        newsWidth = colW;
    }
    auto spans = [&](size_t i, float& before, float& h, float& after) { // the room each kind of line takes
        const Note& n = model.notes[i];
        Note::Kind prev = i ? model.notes[i - 1].kind : Note::kHead;
        switch (n.kind) {
        case Note::kHead: before = i ? kHeadGap : kHeadFirst, h = kHeadRule, after = kHeadAfter; break;
        case Note::kGroup: before = prev == Note::kHead ? 0 : kGroupGap, h = kGroupH, after = kGroupAfter; break;
        case Note::kAside: before = kAsideGap, h = newsHeights[i] + 2 * kAsidePad, after = kAsideAfter; break;
        case Note::kItem: before = 0, h = newsHeights[i], after = kItemAfter; break;
        default: before = 0, h = newsHeights[i], after = kParaAfter; break;
        }
    };
    float content = kTopPad + kEdge;
    for (size_t i = 0; i < model.notes.size(); ++i) {
        float before, h, after;
        spans(i, before, h, after);
        content += before + h + after;
    }
    D2D1_RECT_F view = {colX - kEdge / 2, top + 1, colX + colW + kEdge / 2, panelH};
    if (newsView == 2)
        if (float n = ui.Wheel(view)) newsScroll -= n * kWheel;
    newsScroll = (std::clamp)(newsScroll, 0.0f, (std::max)(0.0f, content - Height(view)));
    float sc = ui.Smooth(Ui::Id("news.scroll"), newsScroll, 18);

    c.PushClip(view);
    float ny = view.top + kTopPad - sc;
    for (size_t i = 0; i < model.notes.size(); ++i) {
        float before, h, after;
        spans(i, before, h, after);
        ny += before;
        if (ny + h >= view.top - kViewSlack && ny <= view.bottom + kViewSlack) { // only what shows
            const Note& n = model.notes[i];
            float q = NewsPart(1 + (int)i), at = ny + (1 - q) * rise;
            IDWriteTextLayout* l = newsLays[i].Get();
            switch (n.kind) {
            case Note::kHead:
                c.TextSp(Upper(n.text), Rect(colX, at, colW, kHeadRule - 6), Weight::Semi, kHeadSize, t.accentHi.Alpha(q), kHeadSp);
                c.Line({colX, at + kHeadRule}, {colX + colW, at + kHeadRule}, t.line.Alpha(q), 1, false);
                break;
            case Note::kGroup: c.Text(n.text, Rect(colX, at, colW, kGroupH), Weight::Semi, kGroupSize, t.text.Alpha(q)); break;
            case Note::kItem:
                c.Dot({colX + kDotX, at + kItemSize * kLeading / 2 + 0.5f}, kDotR, t.accent.Alpha(q));
                Draw(c, l, colX + kItemIndent, at, t.text.Mix(t.textDim, 0.25f).Alpha(q));
                break;
            case Note::kAside:
                c.Fill(Rect(colX, at, colW, h), kAsideR, t.accent.Alpha(0.08f * q));
                c.Fill(Rect(colX, at + kAsidePad, kAsideBar, h - 2 * kAsidePad), kAsideBar / 2, t.accent.Alpha(q));
                Draw(c, l, colX + kAsidePad + kAsideBar, at + kAsidePad, t.textDim.Alpha(q));
                break;
            default: Draw(c, l, colX, at, t.textDim.Alpha(q)); break;
            }
        }
        ny += h + after;
    }
    c.PopClip();
    sheet::FadesIn(c, Ground(t, kTint * (1 - top / (std::max)(panelH, 1.0f))), view, sc, content); // more above or below, fading into the ground
    if (back) NewsShow(1);
    else if (go) NewsEnd();
}
