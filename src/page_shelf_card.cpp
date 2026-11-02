// The shelf's cards: their faces per kind, the previews and text layouts they draw from, made once and let go when unused.
#include <cmath>
#include <unordered_map>

#include "shelf_ui.h"
#include "starfall.h"

namespace shelfui {

namespace {

constexpr size_t kArtMax = 160; // previews held on the device at most; the oldest unused go first
constexpr uint32_t kIdleFrames = 900; // a preview not drawn this many frames is let go
constexpr uint32_t kSweepEvery = 120;
constexpr UINT kSoftPx = 6; // the wash behind a frosted icon: this small, stretched, it's a blur for free
constexpr size_t kCardText = 420; // a card shows the start of a text, never lays out all of it
constexpr size_t kScanMax = 64 * 1024; // search reads this much of a text
constexpr float kFootH = 14;
constexpr size_t kSigChars = 32; // a text's first and last this many characters go into its signature
const Color kWhite{1, 1, 1, 1};

std::unordered_map<uint64_t, Art> arts; // previews on the current device, by thumb key
ID2D1RenderTarget* artTarget = nullptr;
bool forgot = false;
uint32_t frame = 0;

struct Lay {
    ComPtr<IDWriteTextLayout> layout;
    float w = 0, h = 0;
    uint64_t hash = 0;
    uint32_t used = 0;
};
std::unordered_map<uint64_t, Lay> lays; // wrapped text by key: a card's start of text, the open preview's whole
ComPtr<ID2D1SolidColorBrush> ink; // for the layouts drawn here
ComPtr<ID2D1SolidColorBrush> roles[sys::shelfkind::kPunct + 1]; // code colours, set each frame from the theme
std::wstring monoFamily; // a fixed-width face from the system's own fonts, found once
bool monoLooked = false;

uint64_t TextHash(const std::wstring& s) {
    uint64_t h = 1469598103934665603ULL ^ s.size();
    for (wchar_t ch : s) h = (h ^ ch) * 1099511628211ULL;
    return h;
}

IDWriteTextFormat* Mono(float size) {
    static std::unordered_map<int, ComPtr<IDWriteTextFormat>> fmts;
    if (!monoLooked) { // the first fixed-width family, one with mono in its name preferred
        monoLooked = true;
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(gfx::DWrite()->GetSystemFontCollection(&fonts))) {
            std::wstring any;
            for (UINT32 i = 0; i < fonts->GetFontFamilyCount() && monoFamily.empty(); ++i) {
                ComPtr<IDWriteFontFamily> fam;
                ComPtr<IDWriteFont> font;
                ComPtr<IDWriteFont1> f1;
                ComPtr<IDWriteLocalizedStrings> names;
                if (FAILED(fonts->GetFontFamily(i, &fam)) || FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) ||
                    FAILED(font.As(&f1)) || !f1->IsMonospacedFont() || font->IsSymbolFont() || FAILED(fam->GetFamilyNames(&names)))
                    continue;
                wchar_t name[128] = {};
                names->GetString(0, name, 128);
                if (wcsstr(name, L"Mono")) monoFamily = name;
                else if (any.empty()) any = name;
            }
            if (monoFamily.empty()) monoFamily = any;
        }
    }
    int key = (int)std::lround(size * 10);
    auto it = fmts.find(key);
    if (it != fmts.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (monoFamily.empty() || FAILED(gfx::DWrite()->CreateTextFormat(monoFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                                      DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
        return gfx::TextFmt(Weight::Regular, size, Align::Left); // no fixed-width face: the usual one
    fmts[key] = f;
    return f.Get();
}

IDWriteTextLayout* Layout(const std::wstring& s, float w, float h, Weight wt, float size, uint64_t key, bool mono) {
    uint64_t hash = TextHash(s) ^ (uint64_t)(size * 100) << 48 ^ (uint64_t)wt << 40 ^ (mono ? 1ull << 63 : 0);
    Lay& l = lays[key];
    l.used = frame;
    if (l.layout && l.hash == hash && std::fabs(l.w - w) < 0.5f && std::fabs(l.h - h) < 0.5f) return l.layout.Get();
    IDWriteTextFormat* f = mono ? Mono(size) : gfx::TextFmt(wt, size, Align::Left);
    l.layout.Reset();
    if (!f || FAILED(gfx::DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, h, &l.layout))) return nullptr;
    l.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); // the shared format is one line; this layout alone wraps
    l.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    l.w = w, l.h = h, l.hash = hash;
    return l.layout.Get();
}

ID2D1SolidColorBrush* Ink(Canvas& c, Color col) {
    if (!ink) c.Target()->CreateSolidColorBrush(col.D2D(), &ink);
    if (ink) ink->SetColor(col.D2D());
    return ink.Get();
}

struct Facts { // what a card reads out of its text, worked out once per change: a long copy was scanned up to 64k characters every frame
    uint64_t sig = 0;
    std::wstring detail, head;
    sys::shelfkind::Link link;
};
std::unordered_map<uint64_t, Facts> facts; // by card id
constexpr size_t kFactsMax = 512; // past this the lot is dropped and refilled from what's in view

uint64_t Sig(const sys::ShelfItem& it) { // everything those facts come from, cheaply: kind, sizes, the text's ends, the files
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    mix((uint64_t)it.kind);
    mix(it.rgb);
    mix(it.text.size());
    mix((uint64_t)(uintptr_t)it.text.data()); // moved or replaced: a new buffer
    for (size_t i = 0; i < it.text.size() && i < kSigChars; ++i) mix(it.text[i]);
    for (size_t i = it.text.size() > kSigChars ? it.text.size() - kSigChars : it.text.size(); i < it.text.size(); ++i) mix(it.text[i]);
    mix(it.files.size());
    for (const sys::ShelfFile& f : it.files) {
        mix(f.path.size());
        mix((f.dir ? 1u : 0u) | (f.owned ? 2u : 0u));
    }
    return h;
}

std::wstring Head(const std::wstring& s) { // the start of a text, blank lines folded: a card has room for a few lines
    std::wstring out;
    size_t a = s.find_first_not_of(L" \t\r\n");
    for (size_t i = a == std::wstring::npos ? s.size() : a; i < s.size() && out.size() < kCardText; ++i) {
        if (s[i] == L'\r') continue;
        if (s[i] == L'\n' && !out.empty() && out.back() == L'\n') continue;
        out += s[i] == L'\t' ? L' ' : s[i];
    }
    return out;
}

void Foot(Canvas& c, const D2D1_RECT_F& in, int glyph, const wchar_t* mark, const std::wstring& text, Color col) {
    float y = in.bottom - kFootH;
    if (glyph) c.Icon(glyph, {in.left + 6, y + kFootH / 2}, 12, col);
    else c.Text(mark, Rect(in.left, y, 22, kFootH), Weight::Medium, 9.5f, col);
    c.Text(text, Rect(in.left + (glyph ? 16 : 22), y, Width(in) - (glyph ? 16 : 22), kFootH), Weight::Light, 10.5f, col);
}

void Badges(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const sys::ShelfItem& it, float hover, bool overArt) {
    if (it.pinned) {
        D2D1_POINT_2F o = {r.left + 14, r.top + 14};
        c.Dot(o, 10, overArt ? Color{0, 0, 0, 0.45f} : t.accent.Alpha(0.18f));
        PinMark(c, o, 12, overArt ? kWhite : t.accentHi);
    }
    if (it.count > 1) { // added again: a count instead of a copy
        std::wstring n = L"×" + std::to_wstring(it.count);
        float w = c.Measure(n, Weight::Medium, 10) + 12, x = r.right - 8 - w - 24 * hover; // steps aside for the x
        D2D1_RECT_F chip = Rect(x, r.top + 7, w, 16);
        c.Fill(chip, 8, overArt ? Color{0, 0, 0, 0.45f} : t.track);
        c.Text(n, chip, Weight::Medium, 10, overArt ? kWhite : t.textDim, Align::Center);
    }
}

}

View& V() {
    static View v;
    return v;
}

const Art* Thumb(Canvas& c, Model& m, uint64_t id, size_t slot, bool big) {
    if (c.Target() != artTarget) Forget(), artTarget = c.Target(); // another device: nothing made for the old one is any use
    if (forgot) {
        forgot = false;
        m.shelf.ForgetThumbs(); // asked again, for this device
    }
    uint64_t key = sys::Shelf::ThumbKey(id, slot, big);
    auto it = arts.find(key);
    if (it != arts.end()) {
        it->second.used = frame;
        return it->second.bmp ? &it->second : nullptr;
    }
    sys::Shelf::Pixels px;
    if (!m.shelf.TakeThumb(key, px)) {
        m.shelf.WantThumb(key); // made on the worker, posted when ready
        return nullptr;
    }
    Art& a = arts[key];
    a.used = frame;
    a.icon = px.icon;
    if (!px.w || !px.h) return nullptr; // none to be had: the card keeps its glyph, nothing asks again
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1RenderTarget* rt = c.Target();
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(px.w, px.h), px.px.data(), px.w * 4, bp, &a.bmp))) return nullptr;
    uint32_t soft[kSoftPx * kSoftPx];
    for (UINT y = 0; y < kSoftPx; ++y) // box average down to a few pixels
        for (UINT x = 0; x < kSoftPx; ++x) {
            UINT x0 = x * px.w / kSoftPx, x1 = (std::max)(x0 + 1, (x + 1) * px.w / kSoftPx), y0 = y * px.h / kSoftPx, y1 = (std::max)(y0 + 1, (y + 1) * px.h / kSoftPx);
            uint64_t s[4] = {};
            for (UINT yy = y0; yy < y1; ++yy)
                for (UINT xx = x0; xx < x1; ++xx)
                    for (int ch = 0; ch < 4; ++ch) s[ch] += px.px[(size_t)yy * px.w + xx] >> (ch * 8) & 0xFF;
            uint64_t n = (uint64_t)(x1 - x0) * (y1 - y0);
            soft[y * kSoftPx + x] = (uint32_t)(s[0] / n | (s[1] / n) << 8 | (s[2] / n) << 16 | (s[3] / n) << 24);
        }
    rt->CreateBitmap(D2D1::SizeU(kSoftPx, kSoftPx), soft, kSoftPx * 4, bp, &a.soft);
    rt->CreateBitmapBrush(a.bmp.Get(), &a.brush);
    if (a.soft) rt->CreateBitmapBrush(a.soft.Get(), &a.softBrush);
    for (auto* b : {a.brush.Get(), a.softBrush.Get()})
        if (b) b->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return a.brush ? &a : nullptr;
}

void Cover(Canvas& c, ID2D1Bitmap* bmp, ID2D1BitmapBrush* b, const D2D1_RECT_F& r, float radius, float opacity, bool contain) {
    if (!bmp || !b || opacity <= 0 || Width(r) <= 0 || Height(r) <= 0) return;
    D2D1_SIZE_F s = bmp->GetSize();
    if (s.width <= 0 || s.height <= 0) return;
    float k = contain ? (std::min)(Width(r) / s.width, Height(r) / s.height) : (std::max)(Width(r) / s.width, Height(r) / s.height); // contain: whole; else cropped to fill
    float w = s.width * k, h = s.height * k, ox = r.left + (Width(r) - w) / 2, oy = r.top + (Height(r) - h) / 2;
    D2D1_RECT_F box = contain ? D2D1_RECT_F{ox, oy, ox + w, oy + h} : r;
    b->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(ox, oy));
    b->SetOpacity(opacity);
    c.Target()->FillRoundedRectangle({box, radius, radius}, b);
}

void Forget() {
    arts.clear();
    lays.clear(); // code layouts hold this device's brushes
    ink.Reset();
    for (auto& r : roles) r.Reset();
    artTarget = nullptr;
    forgot = true;
}

void Tick(Model& m) {
    if (++frame % kSweepEvery) return;
    for (auto it = arts.begin(); it != arts.end();) {
        bool stale = frame - it->second.used > kIdleFrames || (arts.size() > kArtMax && frame - it->second.used > kSweepEvery);
        if (stale) m.shelf.ForgetThumb(it->first); // scrolled away or gone: made again if it's needed
        it = stale ? arts.erase(it) : std::next(it);
    }
    for (auto it = lays.begin(); it != lays.end();) it = frame - it->second.used > kIdleFrames ? lays.erase(it) : std::next(it);
}

void Para(Canvas& c, const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color col, uint64_t key, bool mono) {
    if (s.empty() || Width(r) <= 0 || Height(r) <= 0) return;
    if (IDWriteTextLayout* l = Layout(s, Width(r), Height(r), w, size, key, mono)) c.Target()->DrawTextLayout({r.left, r.top}, l, Ink(c, col), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float ParaHeight(const std::wstring& s, float width, Weight w, float size, uint64_t key, bool mono) {
    constexpr float kTall = 1e5f;
    IDWriteTextLayout* l = s.empty() ? nullptr : Layout(s, width, kTall, w, size, key, mono);
    DWRITE_TEXT_METRICS tm{};
    return l && SUCCEEDED(l->GetMetrics(&tm)) ? tm.height : 0;
}

void Code(Canvas& c, const Theme& t, const std::wstring& s, D2D1_POINT_2F at, float width, uint64_t key, float& height) {
    constexpr float kSize = 12, kTall = 1e5f;
    namespace sk = sys::shelfkind;
    bool dark = theme::Luma(t.text) > 0.5f;
    float hue = theme::FromCol(t.accent).hue, lit = dark ? 0.74f : 0.36f;
    Color cols[sk::kPunct + 1] = {t.text, t.accentHi, theme::Hsl(hue + 130, 0.45f, lit), t.textLow, theme::Hsl(hue + 60, 0.55f, lit), t.textDim}; // strings and numbers lean off the accent
    for (int i = 0; i <= sk::kPunct; ++i) {
        if (!roles[i]) c.Target()->CreateSolidColorBrush(cols[i].D2D(), &roles[i]);
        if (roles[i]) roles[i]->SetColor(cols[i].D2D());
    }
    auto was = lays.find(key);
    IDWriteTextLayout* before = was == lays.end() ? nullptr : was->second.layout.Get();
    IDWriteTextLayout* l = Layout(s, width, kTall, Weight::Regular, kSize, key, true);
    if (!l) return;
    if (l != before) { // colours applied when the layout is made; a reused one keeps them
        for (const sk::Span& sp : sk::Highlight(s))
            if (roles[sp.role]) l->SetDrawingEffect(roles[sp.role].Get(), {sp.at, sp.len});
    }
    DWRITE_TEXT_METRICS tm{};
    height = SUCCEEDED(l->GetMetrics(&tm)) ? tm.height : 0;
    c.Target()->DrawTextLayout(at, l, roles[0] ? roles[0].Get() : Ink(c, t.text), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Frosted(Canvas& c, const D2D1_RECT_F& r, float radius, const Art* art, int glyph, const Theme& t) {
    bool light = theme::Luma(t.text) < 0.5f; // a light look: a whiter pane
    if (art && art->soft) Cover(c, art->soft.Get(), art->softBrush.Get(), r, radius, 0.9f); // the icon's own colours, blurred, behind the glass
    else c.FillV(r, radius, t.accent.Alpha(0.30f), t.accentHi.Alpha(0.12f));
    c.FillV(r, radius, kWhite.Alpha(light ? 0.50f : 0.16f), kWhite.Alpha(light ? 0.28f : 0.05f));
    c.FillV({r.left, r.top, r.right, r.top + Height(r) * 0.5f}, radius, kWhite.Alpha(0.16f), kWhite.Alpha(0)); // the sheen across its top
    c.StrokeV(r, radius, kWhite.Alpha(light ? 0.7f : 0.38f), kWhite.Alpha(0.06f), 1);
    D2D1_RECT_F inner = Inset(r, Width(r) * 0.17f, Height(r) * 0.17f);
    if (art && art->bmp) Cover(c, art->bmp.Get(), art->brush.Get(), inner, art->icon ? 0.0f : 4.0f, 1, true);
    else c.Icon(glyph, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, Height(r) * 0.42f, t.text);
}

void PinMark(Canvas& c, D2D1_POINT_2F at, float s, Color col) { // a push pin, drawn: the bundled icons have none
    D2D1_POINT_2F head = {at.x + s * 0.12f, at.y - s * 0.16f}, tip = {at.x - s * 0.32f, at.y + s * 0.38f};
    c.Line(head, tip, col, s * 0.11f);
    c.Line({head.x - s * 0.30f, head.y - s * 0.02f}, {head.x + s * 0.06f, head.y + s * 0.34f}, col, s * 0.16f); // the collar, across the needle
    c.Dot({head.x + s * 0.05f, head.y - s * 0.05f}, s * 0.24f, col);
}

int KindGlyph(const sys::ShelfItem& it) {
    using K = sys::ShelfKind;
    switch (it.kind) {
    case K::Link: return kLinkGlyph;
    case K::Image: return glyph::Image;
    case K::File: return it.files.size() == 1 && it.files[0].dir ? glyph::Folder : glyph::File;
    case K::Color: return kColorGlyph;
    default: return glyph::Note;
    }
}

std::wstring Folder(const std::wstring& path) {
    size_t cut = path.find_last_of(L'\\');
    std::wstring parent = cut == std::wstring::npos || cut == 0 ? path : path.substr(0, cut);
    size_t from = parent.find_last_of(L'\\');
    return from == std::wstring::npos ? parent : parent.substr(from + 1);
}

static std::wstring Scan(const sys::ShelfItem& it) { // Detail's own work, done once per change
    using K = sys::ShelfKind;
    if (it.Stack()) return std::to_wstring(it.files.size()) + (it.kind == K::Image ? L" images" : L" files");
    if (it.HasFiles()) {
        const sys::ShelfFile& f = it.files[0];
        if (f.owned) return L"Image, kept by the shelf";
        return (f.dir ? L"Folder in " : L"in ") + Folder(f.path);
    }
    if (it.kind == K::Link) return sys::shelfkind::ReadLink(it.text).type;
    if (it.kind == K::Color) return sys::shelfkind::ColorText(it.rgb, 1);
    size_t lines = 1, words = 0;
    bool inWord = false;
    for (size_t i = 0; i < it.text.size() && i < kScanMax; ++i) {
        wchar_t ch = it.text[i];
        lines += ch == L'\n';
        bool space = ch == L' ' || ch == L'\n' || ch == L'\t' || ch == L'\r';
        words += !space && !inWord;
        inWord = !space;
    }
    if (it.kind == K::Code) return std::to_wstring(lines) + (lines == 1 ? L" line of code" : L" lines of code");
    return words > 1 ? std::to_wstring(words) + L" words" : std::to_wstring(it.text.size()) + L" characters";
}

static const Facts& FactsOf(const sys::ShelfItem& it) {
    uint64_t sig = Sig(it);
    auto found = facts.find(it.id);
    if (found != facts.end() && found->second.sig == sig) return found->second;
    if (facts.size() >= kFactsMax) facts.clear();
    Facts& f = facts[it.id];
    f.sig = sig;
    f.detail = Scan(it);
    f.head = it.HasFiles() ? std::wstring() : Head(it.text);
    f.link = it.kind == sys::ShelfKind::Link ? sys::shelfkind::ReadLink(it.text) : sys::shelfkind::Link{};
    return f;
}

std::wstring Detail(const sys::ShelfItem& it) { return FactsOf(it).detail; }

void Card(PageCtx& p, const D2D1_RECT_F& r, const sys::ShelfItem& it, float hover, bool target) {
    using K = sys::ShelfKind;
    Canvas& c = p.ui.C();
    const Theme& t = p.shell;
    Model& m = p.m;
    if (target) c.Fill(Inset(r, -3, -3), kRadius + 3, t.accent.Alpha(0.28f)); // a carried card would stack here
    c.Fill(r, kRadius, t.cardHi.Mix(t.track, 0.5f * hover));
    D2D1_RECT_F in = Inset(r, 10, 9);
    uint64_t key = it.id << 4;
    const Art* art = it.HasFiles() && !it.Stack() ? Thumb(c, m, it.id, 0, false) : nullptr;
    bool bleed = it.kind == K::Image && art && !art->icon; // a picture fills its card
    Color dim = t.textDim;
    if (bleed) {
        D2D1_RECT_F pic = Inset(r, 1, 1);
        Cover(c, art->bmp.Get(), art->brush.Get(), pic, kRadius - 1, 1);
        c.FillV({pic.left, pic.bottom - Height(pic) * 0.42f, pic.right, pic.bottom}, kRadius - 1, Color{0, 0, 0, 0}, Color{0, 0, 0, 0.62f}); // the name stays legible over any picture
        c.Text(it.Name(), Rect(in.left, in.bottom - 15, Width(in), 16), Weight::Medium, 11.5f, kWhite);
        dim = kWhite.Alpha(0.75f);
    } else if (it.kind == K::Image && it.Stack()) { // a photo grid of the first four
        D2D1_RECT_F g = {r.left + 6, r.top + 6, r.right - 6, r.bottom - 24};
        float cw = (Width(g) - 4) / 2, ch = (Height(g) - 4) / 2;
        for (size_t i = 0; i < 4; ++i) {
            D2D1_RECT_F cell = Rect(g.left + (i % 2) * (cw + 4), g.top + (i / 2) * (ch + 4), cw, ch);
            const Art* a = i < it.files.size() ? Thumb(c, m, it.id, i, false) : nullptr;
            if (a && !a->icon) Cover(c, a->bmp.Get(), a->brush.Get(), cell, i == 0 ? 11.0f : 7.0f, 1);
            else c.Fill(cell, 7, t.track.Alpha(i < it.files.size() ? 1.0f : 0.4f));
        }
        Foot(c, in, glyph::Image, nullptr, Detail(it), dim);
    } else if (it.HasFiles() && it.Stack()) { // a file stack as a short list
        float y = in.top + 1;
        size_t shown = (std::min)(it.files.size(), (size_t)3);
        for (size_t i = 0; i < shown; ++i, y += 22) {
            const Art* a = Thumb(c, m, it.id, i, false);
            D2D1_RECT_F ic = Rect(in.left, y, 18, 18);
            if (a) Cover(c, a->bmp.Get(), a->brush.Get(), ic, 3, 1, true);
            else c.Icon(it.files[i].dir ? glyph::Folder : glyph::File, {ic.left + 9, ic.top + 9}, 15, t.accentHi);
            c.Text(it.files[i].name, Rect(in.left + 24, y, Width(in) - 24, 18), Weight::Regular, 11.5f, t.text);
        }
        std::wstring more = it.files.size() > shown ? L"+" + std::to_wstring(it.files.size() - shown) + L" more, " + Detail(it) : Detail(it);
        Foot(c, in, glyph::File, nullptr, more, dim);
    } else if (it.HasFiles()) { // one file: its icon on frosted glass
        constexpr float kTile = 52;
        float cx = (r.left + r.right) / 2;
        D2D1_RECT_F tile = Rect(cx - kTile / 2, in.top + 2, kTile, kTile);
        Frosted(c, tile, 13, art, KindGlyph(it), t);
        c.Text(it.Name(), Rect(in.left, tile.bottom + 8, Width(in), 17), Weight::Medium, 12, t.text, Align::Center);
        c.Text(Detail(it), Rect(in.left, tile.bottom + 25, Width(in), 14), Weight::Light, 10.5f, dim, Align::Center);
    } else if (it.kind == K::Color) {
        D2D1_RECT_F sw = {in.left, in.top, in.right, in.bottom - kFootH - 20};
        c.Fill(sw, 11, Color::Rgb(it.rgb));
        c.Stroke(sw, 11, kWhite.Alpha(0.18f), 1);
        c.Text(sys::shelfkind::ColorText(it.rgb, 0), Rect(in.left, sw.bottom + 4, Width(in), 16), Weight::Medium, 12.5f, t.text);
        Foot(c, in, kColorGlyph, nullptr, Detail(it), dim);
    } else if (it.kind == K::Link) {
        const sys::shelfkind::Link& l = FactsOf(it).link;
        D2D1_POINT_2F b = {in.left + 11, in.top + 11};
        c.Dot(b, 11, t.accent.Alpha(0.18f));
        c.Icon(kLinkGlyph, b, 14, t.accentHi);
        c.Text(l.site, Rect(in.left + 28, in.top + 2, Width(in) - 28, 18), Weight::Medium, 12, t.text);
        Para(c, l.title, {in.left, in.top + 28, in.right, in.bottom - kFootH - 4}, Weight::Regular, 12, t.text.Mix(t.textDim, 0.2f), key | 1);
        Foot(c, in, 0, L"↗", l.type, dim);
    } else {
        bool code = it.kind == K::Code;
        Para(c, FactsOf(it).head, {in.left, in.top, in.right, in.bottom - kFootH - 4}, Weight::Regular, code ? 10.5f : 12, code ? t.text.Mix(t.textDim, 0.35f) : t.text,
             key | 2, code);
        Foot(c, in, code ? 0 : glyph::Note, L"</>", Detail(it), dim);
    }
    if (!it.note.empty()) { // the user's note takes the footer's place
        D2D1_RECT_F nr = Rect(in.left, in.bottom - kFootH, Width(in), kFootH);
        c.Fill(Inset(nr, -6, -2), 6, bleed ? Color{0, 0, 0, 0.55f} : t.cardHi.Mix(t.accent, 0.10f)); // over the footer it replaces
        c.Icon(glyph::Edit, {nr.left + 6, nr.top + kFootH / 2}, 11, bleed ? kWhite : t.accentHi);
        c.Text(it.note, Rect(nr.left + 16, nr.top, Width(nr) - 16, kFootH), Weight::Regular, 10.5f, bleed ? kWhite : t.accentHi);
    }
    c.StrokeV(r, kRadius, t.hairTop.Mix(t.accent.Alpha(0.6f), (std::max)(hover, target ? 1.0f : 0.0f)), t.hairBot, 1);
    if (t.stars) starfall::Seam(c, {r.left + kRadius, r.top + 0.5f}, {r.right - kRadius, r.top + 0.5f}, t, 0.8f); // starfall: starlight along the top edge, as its cards have
    Badges(c, t, r, it, hover, bleed);
}

void Toast(View& v, double now, const std::wstring& s) {
    v.toast = s;
    v.toastAt = now;
}

bool Matches(const sys::ShelfItem& it, int filter, const std::wstring& query) {
    if (filter > 0 && (int)it.kind != filter - 1) return false;
    if (query.empty()) return true;
    auto has = [&](const std::wstring& s) {
        int n = (int)(std::min)(s.size(), kScanMax);
        return n && FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, s.c_str(), n, query.c_str(),
                                    (int)query.size(), nullptr, nullptr, nullptr, 0) >= 0;
    };
    if (has(it.text) || has(it.note)) return true;
    for (const auto& f : it.files)
        if (has(f.name)) return true;
    if (it.kind == sys::ShelfKind::Link) {
        sys::shelfkind::Link l = sys::shelfkind::ReadLink(it.text);
        return has(l.title) || has(l.site);
    }
    return false;
}

}

void ForgetShelfArt() { shelfui::Forget(); } // the device is going: its bitmaps and brushes with it
