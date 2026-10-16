#include "lyricview.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kInterlude = 4.0; // a gap this long between lines gets the breathing dots
constexpr double kManualSec = 2.5; // after a wheel scroll the lines wait this long, then glide back to the song
constexpr float kAnchor = 0.30f; // the sung line sits this far down the column
constexpr float kDim = 0.34f, kDimLit = 0.42f; // lines out of turn, and the unsung words of the sung line
constexpr float kHazeSigma = 0.11f; // blur of out-of-focus lines, as a share of the type size
constexpr float kHazeRes = 0.5f; // baked at half resolution: it's a blur, nobody can tell, a quarter of the memory
constexpr int kBakesPerFrame = 3; // spreads a new song's baking over a few frames

}

bool LyricView::Lay(Piece& pc, const std::wstring& text, IDWriteTextFormat* f, float lineH) {
    pc = {};
    if (text.empty()) return true;
    if (FAILED(gfx::DWrite()->CreateTextLayout(text.c_str(), (UINT32)text.size(), f, width, 4096, &pc.lay))) return false;
    DWRITE_TRIMMING none = {DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    pc.lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); // long lines wrap, the shared format is single-line
    pc.lay->SetTrimming(&none, nullptr);
    pc.lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    pc.lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, lineH * 0.79f);
    pc.len = (UINT32)text.size();
    UINT32 count = 0;
    pc.lay->GetLineMetrics(nullptr, 0, &count);
    std::vector<DWRITE_LINE_METRICS> lm(count);
    if (count && FAILED(pc.lay->GetLineMetrics(lm.data(), count, &count))) return false;
    UINT32 at = 0;
    for (const auto& m : lm) { // where each wrapped piece sits, so the sweep can run across them in turn
        Span s = {at, m.length - m.trailingWhitespaceLength, 0, 0, pc.h, m.height};
        DWRITE_HIT_TEST_METRICS hm[8];
        UINT32 got = 0;
        if (s.len && SUCCEEDED(pc.lay->HitTestTextRange(at, s.len, 0, 0, hm, 8, &got)) && got) {
            float lo = hm[0].left, hi = hm[0].left + hm[0].width;
            for (UINT32 k = 1; k < got; ++k) {
                lo = (std::min)(lo, hm[k].left);
                hi = (std::max)(hi, hm[k].left + hm[k].width);
            }
            s.left = lo;
            s.width = hi - lo;
        }
        pc.w = (std::max)(pc.w, s.left + s.width);
        pc.spans.push_back(s);
        at += m.length;
        pc.h += m.height;
    }
    return true;
}

void LyricView::Sync(ID2D1DeviceContext* ctx, const std::shared_ptr<const sys::Lyrics>& d, float w, float sz) {
    (void)ctx;
    if (d == doc && std::fabs(w - width) < 0.5f && std::fabs(sz - size) < 0.1f) return;
    doc = d;
    width = w;
    size = sz;
    pad = size * kHazeSigma * 3.2f; // room for the blur to spread
    rows.clear();
    placed = false;
    manual = 0;
    gap = 0;
    focus = 0;
    if (!doc || doc->lines.empty() || width < 40) return;
    IDWriteTextFormat* f = gfx::TextFmt(Weight::Semi, size, Align::Left);
    if (!f) return;
    rows.reserve(doc->lines.size());
    for (const auto& l : doc->lines) {
        Row row;
        row.src = &l;
        if (!Lay(row.lead, l.text, f, size * 1.12f)) continue;
        row.w = row.lead.w;
        row.h = row.lead.h;
        if (row.h > 0) rows.push_back(std::move(row));
    }
}

void LyricView::Ink(ID2D1DeviceContext* ctx, const Row& row, D2D1_POINT_2F at, ID2D1Brush* brush) {
    if (row.lead.lay) ctx->DrawTextLayout(at, row.lead.lay.Get(), brush);
}

void LyricView::Bake(ID2D1DeviceContext* ctx, Row& row) { // the line in white, blurred once into a small texture
    float dx = 96, dy = 96;
    ctx->GetDpi(&dx, &dy);
    float dpi = dx * kHazeRes;
    D2D1_SIZE_U px = {(UINT32)std::ceil((row.w + 2 * pad) * dpi / 96), (UINT32)std::ceil((row.h + 2 * pad) * dpi / 96)};
    if (!px.width || !px.height) return;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    ComPtr<ID2D1Bitmap1> src, out;
    ComPtr<ID2D1Effect> blur;
    if (!ink || FAILED(ctx->CreateBitmap(px, nullptr, 0, bp, &src)) || FAILED(ctx->CreateBitmap(px, nullptr, 0, bp, &out)) ||
        FAILED(ctx->CreateEffect(CLSID_D2D1GaussianBlur, &blur)))
        return;
    ComPtr<ID2D1Image> was;
    ctx->GetTarget(&was);
    ctx->SetDpi(dpi, dpi);
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ink->SetColor(D2D1::ColorF(1, 1, 1, 1));
    ctx->SetTarget(src.Get());
    ctx->BeginDraw();
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    Ink(ctx, row, {pad, pad}, ink.Get());
    bool ok = SUCCEEDED(ctx->EndDraw());
    blur->SetInput(0, src.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, size * kHazeSigma);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY); // once per line, so the nicer one
    ctx->SetTarget(out.Get());
    ctx->BeginDraw();
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ctx->DrawImage(blur.Get());
    ok = SUCCEEDED(ctx->EndDraw()) && ok;
    ctx->SetTarget(was.Get());
    ctx->SetDpi(dx, dy);
    if (ok) row.haze = out;
}

void LyricView::Prepare(ID2D1DeviceContext* ctx) {
    if (rows.empty()) return;
    if (!ink) {
        ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &ink);
        ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &sung);
        ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &unsung);
    }
    const int n = (int)rows.size();
    int baked = 0;
    for (int k = 0, start = (std::max)(0, focus - 3); k < n && baked < kBakesPerFrame; ++k) { // nearest the sung line first, the rest as time allows
        Row& row = rows[(start + k) % n];
        if (row.haze) continue;
        Bake(ctx, row);
        ++baked;
    }
}

float LyricView::Sweep(const Row& row, double t) const { // characters of the lead sung so far
    const sys::LyricLine& l = *row.src;
    const float n = (float)l.text.size();
    if (!l.words.empty()) { // word timing: each word lights on its own beat
        if (t < l.words[0].first) return 0;
        size_t k = 0;
        while (k + 1 < l.words.size() && l.words[k + 1].first <= t) ++k;
        double ws = l.words[k].first, we = k + 1 < l.words.size() ? l.words[k + 1].first : l.end;
        float cs = (float)l.words[k].second, ce = k + 1 < l.words.size() ? (float)l.words[k + 1].second : n;
        double span = (std::max)(0.08, (std::min)(we - ws, 0.12 + (ce - cs) * 0.07));
        return cs + (ce - cs) * anim::Clamp01((float)((t - ws) / span));
    }
    return n * anim::Clamp01((float)((t - l.start) / (std::max)(0.2, l.end - l.start))); // an even sweep over the line's time
}

void LyricView::Haze(ID2D1DeviceContext* ctx, const Row& row, D2D1_POINT_2F at, float scale, Color col) { // the baked blur, tinted
    if (!row.haze || col.a < 0.004f) return;
    ink->SetColor(col.D2D());
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, {at.x, at.y + row.h / 2}));
    D2D1_ANTIALIAS_MODE was = ctx->GetAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED); // opacity masks need it
    D2D1_RECT_F dst = {at.x - pad, at.y - pad, at.x + row.w + pad, at.y + row.h + pad};
    ctx->FillOpacityMask(row.haze.Get(), ink.Get(), &dst, nullptr);
    ctx->SetAntialiasMode(was);
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void LyricView::Text(ID2D1DeviceContext* ctx, const Row& row, D2D1_POINT_2F at, float scale, Color col) {
    if (col.a < 0.004f) return;
    ink->SetColor(col.D2D());
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, {at.x, at.y + row.h / 2}));
    Ink(ctx, row, at, ink.Get());
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void LyricView::Sweeping(ID2D1DeviceContext* ctx, Piece& pc, D2D1_POINT_2F at, float chars, float a, Color text, size_t& e) { // one piece, lit up to chars
    if (!pc.lay) return;
    sung->SetColor(text.Alpha(a).D2D());
    unsung->SetColor(text.Alpha(kDimLit * a).D2D());
    float feather = (pc.spans.empty() ? size : pc.spans[0].height) * 0.7f; // soft edge scales with the type
    for (const Span& s : pc.spans) {
        float f = s.len ? anim::Clamp01((chars - s.first) / s.len) : 1;
        IUnknown* brush = f >= 1 ? sung.Get() : unsung.Get();
        if (f > 0 && f < 1) { // the soft edge of the sweep runs through this piece
            float x = at.x + s.left - feather + f * (s.width + 2 * feather);
            if (e >= edges.size()) {
                ComPtr<ID2D1LinearGradientBrush> g;
                if (FAILED(ctx->CreateLinearGradientBrush({{0, 0}, {1, 0}}, D2D1::BrushProperties(1), stops.Get(), &g))) continue;
                edges.push_back(g);
            }
            ID2D1LinearGradientBrush* g = edges[e++].Get();
            g->SetStartPoint({x - feather, 0});
            g->SetEndPoint({x + feather, 0});
            g->SetOpacity(a);
            brush = g;
        }
        pc.lay->SetDrawingEffect(brush, {s.first, s.len});
    }
    ctx->DrawTextLayout(at, pc.lay.Get(), unsung.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    pc.lay->SetDrawingEffect(nullptr, {0, pc.len}); // plain again for when it's drawn out of turn
}

void LyricView::Lit(ID2D1DeviceContext* ctx, Row& row, D2D1_POINT_2F at, float scale, double t, const Theme& look, float a) {
    if (!(stopsFor.r == look.text.r && stopsFor.g == look.text.g && stopsFor.b == look.text.b)) { // the song's colors changed
        D2D1_GRADIENT_STOP gs[2] = {{0, look.text.Alpha(1).D2D()}, {1, look.text.Alpha(kDimLit).D2D()}};
        stops.Reset();
        edges.clear();
        ctx->CreateGradientStopCollection(gs, 2, &stops);
        stopsFor = look.text;
    }
    if (!stops) return;
    size_t e = 0;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, {at.x, at.y + row.h / 2}));
    Sweeping(ctx, row.lead, at, Sweep(row, t), a, look.text, e);
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void LyricView::Dots(Canvas& c, D2D1_POINT_2F at, float progress, float appear, const Theme& look, float a, double now) {
    float s = appear * (1 + 0.08f * (float)std::sin(now * 2.6)); // the three dots breathe while the music plays on
    float r0 = size * 0.15f * s, step = size * 0.56f * s;
    for (int i = 0; i < 3; ++i) {
        float f = anim::Clamp01(progress * 3 - i); // and fill in turn as the next line nears
        c.Dot({at.x + r0 + i * step, at.y}, r0 * (0.8f + 0.2f * f), look.text.Alpha((0.28f + 0.72f * f) * a * anim::Clamp01(appear)));
    }
}

void LyricView::Waiting(Canvas& c, const D2D1_RECT_F& r, float alpha, const Theme& look, double now) {
    if (size <= 0) return;
    float p = (float)std::fmod(now * 0.7, 1.4); // fill, hold a beat, start over
    Dots(c, {r.left + size * 0.05f, r.top + Height(r) * kAnchor + size * 0.5f}, (std::min)(p, 1.0f), 1, look, alpha * 0.8f, now);
}

double LyricView::Draw(ID2D1DeviceContext* ctx, Canvas& c, Ui& ui, const D2D1_RECT_F& r, double t, float alpha, const Theme& look, bool playing) {
    if (rows.empty() || !ink) return -1;
    float dt = ui.Dt();
    double now = ui.Now();
    const int n = (int)rows.size();
    int a = -1; // the last line that has started
    while (a + 1 < n && rows[a + 1].src->start <= t) ++a;
    int next = a + 1;
    double gs = a < 0 ? 0 : rows[a].src->end, ge = next < n ? rows[next].src->start : 1e9;
    bool inter = next < n && ge - gs >= kInterlude && t >= gs + (a < 0 ? 0 : 0.3);
    int live = !inter && a >= 0 && t < (next < n ? ge : rows[a].src->end + 2.0) ? a : -1;
    focus = inter ? next : (std::max)(a, 0);

    moving = anim::Approach(gap, inter ? size * 1.5f : 0, 9, dt, 0.05f);
    if (float wheel = ui.Wheel(r)) {
        manual += wheel * size * 2.4f;
        manualAt = now;
    }
    bool browsing = now - manualAt < kManualSec;
    if (!browsing) moving |= anim::Approach(manual, 0, 5, dt, 0.5f);

    float rowGap = size * 0.62f, anchor = r.top + Height(r) * kAnchor, y = 0, gapTop = 0;
    tops.resize(n);
    for (int i = 0; i < n; ++i) {
        if (i == next) {
            gapTop = y;
            y += gap;
        }
        tops[i] = y;
        y += rows[i].h + rowGap;
    }
    if (next >= n) gapTop = y;
    float focusY = inter ? gapTop : tops[focus];
    float lo = focusY - (y - rowGap), hi = focusY; // wheel stays within the song
    manual = (std::clamp)(manual, lo - Height(r) * 0.3f, hi + Height(r) * 0.3f);
    for (int i = 0; i < n; ++i) { // every line chases its place; the farther from the sung one, the later it arrives: a ripple
        Row& row = rows[i];
        int d = std::abs(i - focus);
        row.y.target = anchor + tops[i] - focusY + manual;
        row.y.stiff = 150.0f - 11.0f * (std::min)(d, 8);
        row.y.damp = 0.9f;
        if (!placed) row.y.value = row.y.target;
        moving |= row.y.Step(dt);
        moving |= anim::Approach(row.lit, i == live ? 1.0f : 0, i == live ? 12.0f : 2.6f, dt, 0.002f);
        float soft = browsing || row.hover > 0.3f ? 0.0f : d == 0 ? 0.0f : d == 1 ? 0.45f : d == 2 ? 0.75f : 1.0f; // focus falls off with distance
        moving |= anim::Approach(row.blur, soft, 6, dt, 0.005f);
    }
    placed = true;

    double seek = -1;
    c.PushClip({r.left - size, r.top - pad, r.right + size, r.bottom + pad}); // room for the soft edges; the lines fade out before the bounds
    for (int i = 0; i < n; ++i) {
        Row& row = rows[i];
        float y0 = row.y.value;
        if (y0 + row.h < r.top || y0 > r.bottom) continue;
        float top = anim::Clamp01((y0 - r.top) / (Height(r) * 0.20f)), bottom = anim::Clamp01((r.bottom - y0 - row.h) / (Height(r) * 0.24f));
        top = top * top * (3 - 2 * top);
        bottom = bottom * bottom * (3 - 2 * bottom);
        float fade = top * bottom; // gone before it reaches the edge, no hard cut under the clock
        float al = alpha * fade;
        D2D1_RECT_F hit = {r.left - 16, y0 - rowGap * 0.4f, r.left + row.w + 16, y0 + row.h + rowGap * 0.4f};
        Ui::Hit h = al > 0.2f ? ui.Button(Ui::Id("ly.row", i), hit) : Ui::Hit{};
        moving |= anim::Approach(row.hover, h.hover ? 1.0f : 0, 14, dt, 0.01f);
        if (h.clicked) seek = row.src->start; // tap a line to jump there
        if (row.hover > 0.01f) c.Fill(hit, 16, look.text.Alpha(0.07f * row.hover * al));
        if (al < 0.01f) continue;
        float scale = 0.955f + 0.045f * row.lit;
        D2D1_POINT_2F at = {r.left, y0};
        if (i == live) {
            Haze(ctx, row, at, scale, look.accentHi.Alpha(0.55f * al * row.lit)); // the blur in the song's color: a soft bloom behind the sung line
            Lit(ctx, row, at, scale, t, look, al);
            continue;
        }
        int d = std::abs(i - focus);
        float dim = browsing || row.hover > 0.3f ? 0.55f : kDim - 0.04f * (std::min)(d, 4);
        float op = (dim + (1 - dim) * row.lit) * al;
        float b = row.haze ? (std::max)(row.blur * (1 - row.lit), 1 - fade) : 0; // out of focus by distance, and more so as it dissolves at the edges; not baked yet: crisp
        if (b < 0.99f) Text(ctx, row, at, scale, look.text.Alpha(op * (1 - b)));
        if (b > 0.01f) Haze(ctx, row, at, scale, look.text.Alpha((std::min)(1.0f, op * b * 1.2f))); // a blur spreads its ink thinner
    }
    if (gap > 1 && next < n) {
        float p = anim::Clamp01((float)((t - gs) / (std::max)(0.5, ge - gs)));
        Dots(c, {r.left + size * 0.05f, rows[next].y.value - gap / 2 - rowGap * 0.25f}, p, gap / (size * 1.5f), look, alpha, now);
    }
    c.PopClip();
    (void)playing;
    return seek;
}
