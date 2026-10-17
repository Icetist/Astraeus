#include "gfx.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>

#include <d3d11.h>
#include <dxgi1_3.h>

#include <cmath>
#include <utility>

namespace {

struct FmtEntry {
    uint64_t key; // packed weight/size/align, see TextFmt
    ComPtr<IDWriteTextFormat> format;
};

struct State {
    ComPtr<ID2D1Factory3> d2d;
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteFontCollection1> textFonts, iconLine, iconFill; // private bundled fonts, not installed
    std::wstring textFam = L"Segoe UI Variable Text"; // fallback if outfit fails to load
    std::wstring lineFam, fillFam;
    std::vector<FmtEntry> formats; // few entries, linear scan is fine
    ComPtr<IDWriteInMemoryFontFileLoader> mem; // serves fonts straight from the exe image
};

State* g = nullptr; // process-wide, created in Init

struct Pin : IUnknown { // owner for in-memory fonts: resource memory lives as long as the exe, so no refcount
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = id == __uuidof(IUnknown) ? this : nullptr;
        return *out ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
} gPin;

ComPtr<IDWriteFontCollection1> LoadRes(std::initializer_list<int> ids) { // fonts embedded as RCDATA, zero copies
    if (!g->mem) return nullptr;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(g->dw->CreateFontSetBuilder(&builder))) return nullptr;
    int added = 0;
    for (int id : ids) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        HGLOBAL h = r ? LoadResource(nullptr, r) : nullptr;
        const void* data = h ? LockResource(h) : nullptr;
        if (!data) continue;
        ComPtr<IDWriteFontFile> file; // owner set, so dwrite reads the mapped image instead of copying
        if (SUCCEEDED(g->mem->CreateInMemoryFontFileReference(g->dw.Get(), data, SizeofResource(nullptr, r), &gPin, &file)) &&
            SUCCEEDED(builder->AddFontFile(file.Get())))
            ++added;
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (added == 0 || FAILED(builder->CreateFontSet(&set)) || FAILED(g->dw->CreateFontCollectionFromFontSet(set.Get(), &collection)))
        return nullptr;
    return collection;
}

ComPtr<IDWriteFontCollection1> LoadFonts(const std::vector<std::wstring>& files) {
    ComPtr<IDWriteFontSetBuilder1> builder; // in-memory font set from loose ttf files
    if (FAILED(g->dw->CreateFontSetBuilder(&builder))) return nullptr;
    int added = 0;
    for (const auto& path : files) {
        ComPtr<IDWriteFontFile> file;
        if (FAILED(g->dw->CreateFontFileReference(path.c_str(), nullptr, &file))) continue; // skip missing files
        if (SUCCEEDED(builder->AddFontFile(file.Get()))) ++added;
    }
    if (added == 0) return nullptr; // caller falls back to system font
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(&set))) return nullptr;
    if (FAILED(g->dw->CreateFontCollectionFromFontSet(set.Get(), &collection))) return nullptr;
    return collection;
}

std::wstring FirstFam(IDWriteFontCollection1* c, const wchar_t* prefer) {
    if (!c) return L"";
    std::wstring first; // fallback when preferred name absent
    for (UINT32 i = 0; i < c->GetFontFamilyCount(); ++i) {
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(c->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&names))) continue;
        wchar_t name[128] = {};
        names->GetString(0, name, 128); // first locale name is enough
        if (first.empty()) first = name;
        if (prefer && wcscmp(name, prefer) == 0) return name;
    }
    return first;
}

IDWriteTextFormat* Cached(uint64_t key, IDWriteFontCollection* collection, const std::wstring& family,
                          DWRITE_FONT_WEIGHT weight, float size, DWRITE_TEXT_ALIGNMENT align) {
    for (const auto& e : g->formats)
        if (e.key == key) return e.format.Get(); // reuse, formats never evicted
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(g->dw->CreateTextFormat(family.c_str(), collection, weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
        return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); // vertically centered in rect
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); // single line labels
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(g->dw->CreateEllipsisTrimmingSign(f.Get(), &ellipsis))) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; // overflow ends in "..."
        f->SetTrimming(&trim, ellipsis.Get());
    }
    g->formats.push_back({key, f});
    return f.Get();
}

DWRITE_TEXT_ALIGNMENT ToDw(Align a) {
    return a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : a == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING; // trailing = right in ltr
}

}

namespace gfx {

ComPtr<ID2D1Bitmap> BakeBlur(ID2D1DeviceContext* ctx, ID2D1Bitmap1* source, float sigma) {
    ComPtr<ID2D1Effect> blur;
    ComPtr<ID2D1Bitmap1> out;
    ComPtr<ID2D1Image> was;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(ctx->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) || FAILED(ctx->CreateBitmap(source->GetPixelSize(), nullptr, 0, bp, &out))) return nullptr;
    blur->SetInput(0, source);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD); // no dark fade at edges
    float dx, dy;
    ctx->GetDpi(&dx, &dy);
    ctx->GetTarget(&was);
    ctx->SetDpi(96, 96);
    ctx->SetTarget(out.Get()); // bake once, not every frame
    ctx->BeginDraw();
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ctx->DrawImage(blur.Get());
    ctx->EndDraw();
    ctx->SetTarget(was.Get());
    ctx->SetDpi(dx, dy);
    return out;
}

bool Init(const std::wstring& fontDir) {
    if (g) return true;
    g = new State();
    D2D1_FACTORY_OPTIONS opts = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &opts, &g->d2d))) return false; // ui thread only
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), &g->dw))) return false; // shared = system font cache
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g->wic)))) return false; // needs CoInitialize first

    if (SUCCEEDED(g->dw->CreateInMemoryFontFileLoader(&g->mem)) && FAILED(g->dw->RegisterFontFileLoader(g->mem.Get())))
        g->mem.Reset();
    g->textFonts = LoadRes({201, 202, 203, 204, 205}); // ids from app.rc, outfit 200..600
    g->iconLine = LoadRes({206});
    g->iconFill = LoadRes({207});
    std::wstring dir = fontDir + L"\\"; // loose files only as a fallback
    if (!g->textFonts) g->textFonts = LoadFonts({dir + L"Outfit-200.ttf", dir + L"Outfit-300.ttf", dir + L"Outfit-400.ttf", // one file per Weight value
                                   dir + L"Outfit-500.ttf", dir + L"Outfit-600.ttf"});
    if (!g->iconLine) g->iconLine = LoadFonts({dir + L"SymbolsOutline.ttf"});
    if (!g->iconFill) g->iconFill = LoadFonts({dir + L"SymbolsFilled.ttf"});
    if (g->textFonts) g->textFam = FirstFam(g->textFonts.Get(), L"Outfit");
    g->lineFam = FirstFam(g->iconLine.Get(), nullptr); // icon fonts have one family, take it
    g->fillFam = FirstFam(g->iconFill.Get(), nullptr);
    if (!g->iconLine || !g->iconFill) OutputDebugStringW(L"Astraeus: icon fonts missing\n"); // non-fatal, icons render as tofu
    return true;
}

void Shutdown() {
    if (g && g->mem) g->dw->UnregisterFontFileLoader(g->mem.Get());
    delete g;
    g = nullptr;
}

void Settle(ID3D11Device* d) {
    if (!d) return;
    ComPtr<ID3D11DeviceContext> ic;
    d->GetImmediateContext(&ic);
    if (ic) {
        ic->ClearState(); // unbind everything so the driver can free it now, not at some later flush
        ic->Flush();
    }
    ComPtr<IDXGIDevice3> dx;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&dx)))) dx->Trim(); // drops the driver's internal buffers
}

void GiveBack() {
    HEAP_OPTIMIZE_RESOURCES_INFORMATION info{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    HeapSetInformation(nullptr, HeapOptimizeResources, &info, sizeof(info)); // decommit free heap blocks
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0); // then trim what's left idle
}

ID2D1Factory3* D2D() { return g->d2d.Get(); }
IDWriteFactory5* DWrite() { return g->dw.Get(); }
IWICImagingFactory* Wic() { return g->wic.Get(); }

IDWriteTextFormat* TextFmt(Weight weight, float size, Align align) {
    uint64_t key = (uint64_t)weight << 40 | (uint64_t)(size * 10) << 8 | (uint64_t)align << 1; // size in 0.1 steps
    return Cached(key, g->textFonts.Get(), g->textFam, (DWRITE_FONT_WEIGHT)weight, size, ToDw(align));
}

IDWriteTextFormat* IconFmt(float size, bool filled) {
    uint64_t key = (1ull << 60) | (filled ? 1ull << 59 : 0) | (uint64_t)(size * 10) << 8; // bit 60 keeps icons apart from text keys
    return filled ? Cached(key, g->iconFill.Get(), g->fillFam, DWRITE_FONT_WEIGHT_LIGHT, size, DWRITE_TEXT_ALIGNMENT_CENTER)
                  : Cached(key, g->iconLine.Get(), g->lineFam, DWRITE_FONT_WEIGHT_LIGHT, size, DWRITE_TEXT_ALIGNMENT_CENTER);
}

static ComPtr<IWICBitmapSource> Finish(IWICBitmapFrameDecode* frame, UINT maxPx) {
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return nullptr;
    ComPtr<IWICBitmapSource> source = frame;
    if (w > maxPx || h > maxPx) {
        float s = (float)maxPx / (w > h ? w : h); // fit longest side, keep aspect
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(g->wic->CreateBitmapScaler(&scaler))) return nullptr;
        if (FAILED(scaler->Initialize(frame, (UINT)(w * s + 0.5f), (UINT)(h * s + 0.5f), WICBitmapInterpolationModeFant))) return nullptr; // fant = good downscale quality
        source = scaler;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g->wic->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return nullptr; // premultiplied bgra, what d2d wants
    ComPtr<IWICBitmap> cached;
    if (FAILED(g->wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &cached))) return nullptr; // decode now, drop source stream
    return cached;
}

ComPtr<IWICBitmapSource> Decode(const uint8_t* data, size_t size, UINT maxPx) {
    if (!data || size == 0) return nullptr;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(g->wic->CreateStream(&stream))) return nullptr;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)size))) return nullptr; // wraps buffer, no copy
    if (FAILED(g->wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return nullptr; // format sniffed from bytes
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr; // first frame only, no animation
    return Finish(frame.Get(), maxPx);
}

static ComPtr<IWICBitmapSource> FastDecode(IWICBitmapFrameDecode* frame, UINT maxPx) {
    ComPtr<IWICBitmapSourceTransform> transform;
    UINT w = 0, h = 0;
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&transform))) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
        return nullptr;
    float s = (float)maxPx / (w > h ? w : h);
    if (s >= 1) return nullptr; // already small, normal path is fine
    UINT tw = (UINT)(w * s) + 1, th = (UINT)(h * s) + 1; // +1 so closest size never undershoots
    if (FAILED(transform->GetClosestSize(&tw, &th))) return nullptr; // jpeg decodes at 1/2, 1/4, 1/8 for free
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(transform->GetClosestPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) return nullptr; // only handle plain bgr
    UINT stride = (tw * 3 + 3) & ~3u; // rows padded to 4 bytes
    std::vector<BYTE> pixels((size_t)stride * th);
    if (FAILED(transform->CopyPixels(nullptr, tw, th, &fmt, WICBitmapTransformRotate0, stride, (UINT)pixels.size(), pixels.data())))
        return nullptr;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(g->wic->CreateBitmapFromMemory(tw, th, GUID_WICPixelFormat24bppBGR, stride, (UINT)pixels.size(), pixels.data(), &bitmap)))
        return nullptr;
    return bitmap;
}

ComPtr<IWICBitmapSource> DecodeFile(const wchar_t* path, UINT maxPx) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(g->wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return nullptr;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    if (ComPtr<IWICBitmapSource> scaled = FastDecode(frame.Get(), maxPx)) { // big wallpapers: scale during decode
        ComPtr<IWICFormatConverter> conv; // only convert, fast path already sized
        if (SUCCEEDED(g->wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaled.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
            return conv;
    }
    return Finish(frame.Get(), maxPx); // fallback: full decode + scaler
}

}

Canvas::Canvas(ID2D1RenderTarget* target) : rt(target) {
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush); // one brush, recolored per draw
    D2D1_STROKE_STYLE_PROPERTIES p = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, // round caps for arcs and lines
                                      D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f};
    gfx::D2D()->CreateStrokeStyle(p, nullptr, 0, &rndCaps);
}

ID2D1SolidColorBrush* Canvas::Brush(Color c) {
    brush->SetColor(c.D2D());
    return brush.Get();
}

void Canvas::Fill(const D2D1_RECT_F& r, float radius, Color c) {
    if (c.a <= 0) return;
    if (radius <= 0) rt->FillRectangle(r, Brush(c));
    else rt->FillRoundedRectangle({r, radius, radius}, Brush(c));
}

void Canvas::Stroke(const D2D1_RECT_F& r, float radius, Color c, float width) {
    D2D1_RECT_F in = Inset(r, width / 2, width / 2); // keep stroke inside rect bounds
    rt->DrawRoundedRectangle({in, radius, radius}, Brush(c), width);
}

void Canvas::Dot(D2D1_POINT_2F center, float radius, Color c) {
    rt->FillEllipse({center, radius, radius}, Brush(c));
}

void Canvas::Ring(D2D1_POINT_2F center, float radius, Color c, float width) {
    rt->DrawEllipse({center, radius, radius}, Brush(c), width);
}

void Canvas::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, Color c, float width, bool round) {
    rt->DrawLine(a, b, Brush(c), width, round ? rndCaps.Get() : nullptr);
}

void Canvas::Arc(D2D1_POINT_2F c, float r, float from, float sweep, Color color, float width) {
    if (sweep <= 0.0005f) return;
    if (sweep >= 6.2826f) { // ~2pi, arc segment can't close itself
        Ring(c, r, color, width);
        return;
    }
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    D2D1_POINT_2F start = {c.x + r * std::cos(from), c.y + r * std::sin(from)}; // angles in radians, 0 = 3 o'clock
    D2D1_POINT_2F end = {c.x + r * std::cos(from + sweep), c.y + r * std::sin(from + sweep)};
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc({end, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, sweep > 3.14159f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL}); // large flag past half circle
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(path.Get(), Brush(color), width, rndCaps.Get());
}

void Canvas::FillGeometry(ID2D1Geometry* geometry, Color c) {
    rt->FillGeometry(geometry, Brush(c));
}

namespace {

uint64_t Mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); // boost-style hash_combine, golden ratio const
    return h;
}

uint64_t ColKey(Color c) {
    auto q = [](float v) { return (uint64_t)(v * 255.0f + 0.5f) & 0xFF; }; // quantize to 8 bit so near colors share
    return q(c.r) | q(c.g) << 8 | q(c.b) << 16 | q(c.a) << 24;
}

uint64_t HashText(const std::wstring& s, const void* f, float w, float h, float spacing) {
    uint64_t k = 1469598103934665603ull; // fnv-1a offset basis
    for (wchar_t ch : s) k = (k ^ (uint64_t)ch) * 1099511628211ull; // fnv-1a over text
    k = Mix64(k, (uint64_t)(uintptr_t)f); // format pointer is stable, formats never freed
    k = Mix64(k, (uint64_t)(w * 4)); // quarter px precision
    k = Mix64(k, (uint64_t)(h * 4));
    return Mix64(k, (uint64_t)(spacing * 16));
}

constexpr size_t kMaxGrad = 96; // gradient cache cap, wiped when full
constexpr uint32_t kLayTtl = 240; // frames a layout may sit unused before eviction

}

ID2D1LinearGradientBrush* Canvas::Gradient(Color a, Color b, bool vertical, const D2D1_RECT_F& r) {
    uint64_t key = ColKey(a) | ColKey(b) << 32; // keyed by colors only, not rect
    key = Mix64(key, vertical ? 1 : 2);
    auto it = grads.find(key);
    if (it == grads.end()) {
        if (grads.size() >= kMaxGrad) grads.clear(); // crude eviction, cheap to rebuild
        D2D1_GRADIENT_STOP stops[2] = {{0, a.D2D()}, {1, b.D2D()}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> created;
        if (FAILED(rt->CreateGradientStopCollection(stops, 2, &collection))) return nullptr;
        D2D1_POINT_2F end = vertical ? D2D1_POINT_2F{0, 1} : D2D1_POINT_2F{1, 0}; // unit gradient, stretched below
        if (FAILED(rt->CreateLinearGradientBrush({{0, 0}, end}, collection.Get(), &created))) return nullptr;
        it = grads.emplace(key, created).first;
    }
    float sx = vertical ? 1 : Width(r), sy = vertical ? Height(r) : 1;
    it->second->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(r.left, r.top)); // map unit gradient onto rect
    return it->second.Get();
}

IDWriteTextLayout* Canvas::Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h, float spacing) {
    uint64_t key = HashText(s, f, w, h, spacing);
    auto range = layouts.equal_range(key); // multimap, tolerate hash collisions
    for (auto it = range.first; it != range.second; ++it) {
        TextLay& e = it->second;
        if (e.format == f && e.width == w && e.height == h && e.spacing == spacing && e.text == s) { // full compare to confirm hit
            e.lastUse = frame; // keep alive for ttl
            return e.layout.Get();
        }
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(gfx::DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, h, &layout))) return nullptr;
    if (spacing != 0) {
        ComPtr<IDWriteTextLayout1> l1;
        if (SUCCEEDED(layout.As(&l1))) l1->SetCharacterSpacing(0, spacing, 0, {0, (UINT32)s.size()}); // tracking needs layout1
    }
    auto it = layouts.emplace(key, TextLay{s, f, w, h, spacing, layout, frame});
    return it->second.layout.Get();
}

void Canvas::NewFrame() {
    ++frame;
    if (frame % 120 != 0) return; // sweep every 120 frames, not every frame
    for (auto it = layouts.begin(); it != layouts.end();) {
        if (frame - it->second.lastUse > kLayTtl) it = layouts.erase(it); // evict unused after ttl
        else ++it;
    }
}

void Canvas::FillV(const D2D1_RECT_F& r, float radius, Color top, Color bottom) {
    if (auto* b = Gradient(top, bottom, true, r)) rt->FillRoundedRectangle({r, radius, radius}, b);
}

void Canvas::StrokeV(const D2D1_RECT_F& r, float radius, Color top, Color bottom, float width) {
    D2D1_RECT_F in = Inset(r, width / 2, width / 2);
    if (auto* b = Gradient(top, bottom, true, r)) rt->DrawRoundedRectangle({in, radius, radius}, b, width); // gradient spans outer rect, stroke inset
}

void Canvas::FillH(const D2D1_RECT_F& r, float radius, Color left, Color right) {
    if (auto* b = Gradient(left, right, false, r)) rt->FillRoundedRectangle({r, radius, radius}, b);
}

void Canvas::FillGeoV(ID2D1Geometry* geometry, const D2D1_RECT_F& bounds, Color top, Color bottom) {
    if (auto* b = Gradient(top, bottom, true, bounds)) rt->FillGeometry(geometry, b);
}

void Canvas::Glow(D2D1_POINT_2F center, float rx, float ry, Color c) {
    if (c.a <= 0.003f || rx <= 0 || ry <= 0) return; // below 1/255, invisible
    uint64_t key = ColKey(c.Alpha(1 / c.a)); // key on opaque color, alpha applied via opacity
    auto it = glows.find(key);
    if (it == glows.end()) {
        if (glows.size() >= 32) glows.clear(); // small cap, few glow colors per theme
        Color solid = c.Alpha(1 / c.a);
        D2D1_GRADIENT_STOP stops[3] = {{0, solid.D2D()}, {0.45f, solid.Alpha(0.45f).D2D()}, {1, solid.Alpha(0).D2D()}}; // mid stop gives softer falloff than linear
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> created;
        if (FAILED(rt->CreateGradientStopCollection(stops, 3, &collection)) ||
            FAILED(rt->CreateRadialGradientBrush({{0, 0}, {0, 0}, 1, 1}, collection.Get(), &created))) // unit circle at origin
            return;
        it = glows.emplace(key, created).first;
    }
    it->second->SetOpacity(c.a);
    it->second->SetTransform(D2D1::Matrix3x2F::Scale(rx, ry) * D2D1::Matrix3x2F::Translation(center.x, center.y)); // stretch unit circle to ellipse
    rt->FillEllipse({center, rx, ry}, it->second.Get());
}

void Canvas::StrokeGeo(ID2D1Geometry* geometry, Color c, float width) {
    rt->DrawGeometry(geometry, Brush(c), width, rndCaps.Get());
}

void Canvas::Text(const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color c, Align a) {
    if (s.empty()) return;
    IDWriteTextFormat* f = gfx::TextFmt(w, size, a);
    IDWriteTextLayout* l = f ? Layout(s, f, Width(r), Height(r), 0) : nullptr; // cached layout, no reshaping per frame
    if (l) rt->DrawTextLayout({r.left, r.top}, l, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP); // clip to rect
}

float Canvas::TextOnce(const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color c) {
    IDWriteTextFormat* f = gfx::TextFmt(w, size, Align::Left);
    ComPtr<IDWriteTextLayout> l;
    DWRITE_TEXT_METRICS tm{};
    if (s.empty() || !f || FAILED(gfx::DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, Width(r), Height(r), &l))) return 0;
    rt->DrawTextLayout({r.left, r.top}, l.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return SUCCEEDED(l->GetMetrics(&tm)) ? tm.widthIncludingTrailingWhitespace : 0;
}

void Canvas::TextSp(const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color c, float spacing, Align a) {
    if (s.empty()) return;
    IDWriteTextFormat* f = gfx::TextFmt(w, size, a);
    IDWriteTextLayout* l = f ? Layout(s, f, Width(r), Height(r), spacing) : nullptr;
    if (l) rt->DrawTextLayout({r.left, r.top}, l, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Canvas::Icon(int glyph, D2D1_POINT_2F center, float size, Color c, bool filled) {
    wchar_t ch = (wchar_t)glyph; // codepoints are bmp, fits one wchar
    IDWriteTextFormat* f = gfx::IconFmt(size, filled);
    if (!f) return;
    D2D1_RECT_F r = {center.x - size, center.y - size, center.x + size, center.y + size}; // 2x box, centered format does the rest
    rt->DrawTextW(&ch, 1, f, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

ID2D1BitmapBrush* Canvas::BitmapBrush(ID2D1Bitmap* bmp) {
    for (auto& [key, b] : bmpBrushes)
        if (key == bmp) return b.Get(); // brush holds a ref, so the pointer can't be reused while cached
    ComPtr<ID2D1BitmapBrush> b;
    if (FAILED(rt->CreateBitmapBrush(bmp, &b))) return nullptr; // brush lets us fill a rounded rect
    b->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    constexpr size_t kMaxBmpBrushes = 2; // a frame only draws the cover and its blur
    if (bmpBrushes.size() >= kMaxBmpBrushes) bmpBrushes.erase(bmpBrushes.begin()); // drops the old cover's gpu copy too
    bmpBrushes.emplace_back(bmp, b);
    return b.Get();
}

void Canvas::Image(ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float radius, float opacity) {
    if (!bmp || opacity <= 0) return;
    D2D1_SIZE_F s = bmp->GetSize();
    if (s.width <= 0 || s.height <= 0) return;
    float scale = (std::max)(Width(r) / s.width, Height(r) / s.height); // cover fit, crop overflow
    float ox = r.left + (Width(r) - s.width * scale) / 2;
    float oy = r.top + (Height(r) - s.height * scale) / 2;
    ID2D1BitmapBrush* b = BitmapBrush(bmp);
    if (!b) return;
    b->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(ox, oy));
    b->SetOpacity(opacity);
    rt->FillRoundedRectangle({r, radius, radius}, b);
}

void Canvas::Art(ID2D1Bitmap* bmp, ID2D1Bitmap* backdrop, const D2D1_RECT_F& r, float radius, float opacity) {
    if (!bmp || opacity <= 0) return;
    D2D1_SIZE_F s = bmp->GetSize();
    if (s.width <= 0 || s.height <= 0 || Width(r) <= 0 || Height(r) <= 0) return;
    const float kSquarish = 0.15f; // album covers: within 15% of the box shape, crop to fill
    float rel = (s.width / s.height) / (Width(r) / Height(r));
    if (rel > 1 - kSquarish && rel < 1 + kSquarish) {
        Image(bmp, r, radius, opacity);
        return;
    }
    Image(backdrop ? backdrop : bmp, r, radius, opacity); // video frames: blurred copy fills the box
    Fill(r, radius, Color{0, 0, 0, 0.25f * opacity}); // dim it so the real frame stands out
    float scale = (std::min)(Width(r) / s.width, Height(r) / s.height); // contain fit, nothing cropped
    float w = s.width * scale, h = s.height * scale;
    D2D1_RECT_F fit = {r.left + (Width(r) - w) / 2, r.top + (Height(r) - h) / 2, 0, 0};
    fit.right = fit.left + w;
    fit.bottom = fit.top + h;
    Image(bmp, fit, radius * 0.5f, opacity);
}

void Canvas::PushClip(const D2D1_RECT_F& r) {
    rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void Canvas::PopClip() {
    rt->PopAxisAlignedClip();
}
