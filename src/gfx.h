#pragma once
#include <windows.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

struct ID3D11Device;

struct Color {
    float r = 0, g = 0, b = 0, a = 1; // straight alpha, 0..1

    static Color Rgb(uint32_t rgb, float alpha = 1) { // from 0xRRGGBB hex
        return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
    }
    Color Alpha(float alpha) const { return {r, g, b, a * alpha}; } // multiplies, not replaces
    Color Mix(const Color& o, float t) const {
        return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t}; // linear lerp in srgb space
    }
    D2D1_COLOR_F D2D() const { return {r, g, b, a}; }
};

enum class Weight { Thin = 200, Light = 300, Regular = 400, Medium = 500, Semi = 600 }; // values = dwrite weights, cast directly
enum class Align { Left, Center, Right };

namespace gfx {

bool Init(const std::wstring& fontDir); // factories + bundled fonts, call once after CoInitialize
void Shutdown();
void Settle(ID3D11Device* d); // before a device goes: finish its work and let the driver drop its scratch memory
void GiveBack(); // freed heap and idle pages back to windows; only when nothing is animating, it costs page faults after
ID2D1Factory3* D2D();
IDWriteFactory5* DWrite();
IWICImagingFactory* Wic();

IDWriteTextFormat* TextFmt(Weight weight, float size, Align align); // cached, pointer stays valid until Shutdown
IDWriteTextFormat* LyricFmt(float size); // the lyrics' rounder face, left aligned
IDWriteTextFormat* IconFmt(float size, bool filled); // outline or filled symbol font

ComPtr<IWICBitmapSource> Decode(const uint8_t* data, size_t size, UINT maxPx); // downscaled to maxPx, pbgra out
ComPtr<IWICBitmapSource> DecodeFile(const wchar_t* path, UINT maxPx); // tries scaled jpeg decode first
ComPtr<ID2D1Bitmap> BakeBlur(ID2D1DeviceContext* ctx, ID2D1Bitmap1* source, float sigma); // blurred copy, baked once

}

class Canvas {
public:
    explicit Canvas(ID2D1RenderTarget* target);

    ID2D1RenderTarget* Target() const { return rt; }

    void Fill(const D2D1_RECT_F& r, float radius, Color c);
    void Stroke(const D2D1_RECT_F& r, float radius, Color c, float width);
    void Dot(D2D1_POINT_2F center, float radius, Color c);
    void Ring(D2D1_POINT_2F center, float radius, Color c, float width);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, Color c, float width, bool round = true);
    void Arc(D2D1_POINT_2F center, float radius, float from, float sweep, Color c, float width);
    // kept: g stays the same object frame after frame (the panel's shape, a sparkline), so its tessellation is cached on the gpu instead of redone per draw
    void FillGeometry(ID2D1Geometry* g, Color c, bool kept = false);
    void FillGeoV(ID2D1Geometry* g, const D2D1_RECT_F& bounds, Color top, Color bottom, bool kept = false);
    void StrokeGeo(ID2D1Geometry* g, Color c, float width, bool kept = false);
    void FillGeo(ID2D1Geometry* g, ID2D1Brush* b, bool kept = false); // any brush, e.g. a gradient the caller keeps
    void StrokeGeo(ID2D1Geometry* g, ID2D1Brush* b, float width, ID2D1StrokeStyle* style, bool kept = false); // style null: square ends, as DrawGeometry has
    ID2D1StrokeStyle* RoundCaps() const { return rndCaps.Get(); }
    void Glow(D2D1_POINT_2F center, float rx, float ry, Color c, const D2D1_ROUNDED_RECT* in = nullptr); // radial falloff, cached per color; in: kept inside a card's rounded corners
    void NewFrame(); // advances frame counter, sweeps stale layouts
    struct Word { // something written or an icon drawn, for a screen reader; TextOnce's typed text is never among them
        std::wstring text;
        int glyph; // 0: words
        D2D1_RECT_F r; // on the target
    };
    void Listen(bool on) { listening = on; } // a reader is here: remember this frame's words
    const std::vector<Word>& Words() const { return words; }
    void FillV(const D2D1_RECT_F& r, float radius, Color top, Color bottom); // V = vertical gradient, H = horizontal
    void StrokeV(const D2D1_RECT_F& r, float radius, Color top, Color bottom, float width);
    void FillH(const D2D1_RECT_F& r, float radius, Color left, Color right);

    void Text(const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color c, Align a = Align::Left);
    float TextOnce(const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color c); // never cached (secrets); returns the drawn width
    float Measure(const std::wstring& s, Weight w, float size); // a single line's width in dips, from the same cached layouts Text draws
    void TextSp(const std::wstring& s, const D2D1_RECT_F& r, Weight w, float size, Color c, float spacing, Align a = Align::Left);
    void Icon(int glyph, D2D1_POINT_2F center, float size, Color c, bool filled = false);

    void Image(ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float radius, float opacity = 1); // cover fit, rounded
    void Art(ID2D1Bitmap* bmp, ID2D1Bitmap* backdrop, const D2D1_RECT_F& r, float radius, float opacity = 1); // square art fills, wide art shown whole over backdrop

    void PushClip(const D2D1_RECT_F& r);
    void PopClip();

private:
    struct TextLay {
        std::wstring text;
        IDWriteTextFormat* format;
        float width, height, spacing;
        ComPtr<IDWriteTextLayout> layout;
        uint32_t lastUse; // frame index, for ttl eviction
    };

    ID2D1SolidColorBrush* Brush(Color c);
    ID2D1LinearGradientBrush* Gradient(Color a, Color b, bool vertical, const D2D1_RECT_F& r);
    IDWriteTextLayout* Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h, float spacing);

    ID2D1RenderTarget* rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> rndCaps;
    std::unordered_map<uint64_t, ComPtr<ID2D1LinearGradientBrush>> grads;
    std::unordered_map<uint64_t, ComPtr<ID2D1RadialGradientBrush>> glows;
    std::unordered_multimap<uint64_t, TextLay> layouts; // multimap: hash collisions resolved by full compare
    std::vector<std::pair<ID2D1Bitmap*, ComPtr<ID2D1BitmapBrush>>> bmpBrushes; // cover + its blur, oldest evicted
    ID2D1BitmapBrush* BitmapBrush(ID2D1Bitmap* bmp);
    struct Real { // a geometry's cached tessellation, made for this device
        ComPtr<ID2D1Geometry> geo; // held so its address can't be reused by another while cached
        ComPtr<ID2D1GeometryRealization> real;
        float stroke; // 0 a fill, else the stroke width
        ID2D1StrokeStyle* style;
        float tol; // flattening tolerance it was made for
        uint32_t lastUse;
    };
    ID2D1GeometryRealization* Realize(ID2D1Geometry* g, float stroke, ID2D1StrokeStyle* style);
    ComPtr<ID2D1DeviceContext1> dc1; // null on a target without realizations: kept draws go the plain way
    std::vector<Real> reals;
    uint32_t frame = 0;
    void Say(const std::wstring& s, int glyph, const D2D1_RECT_F& r);
    bool listening = false;
    std::vector<Word> words;
};

inline D2D1_RECT_F Rect(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
inline D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy) { return {r.left + dx, r.top + dy, r.right - dx, r.bottom - dy}; }
inline bool Inside(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; } // half-open, adjacent rects never both hit
inline float Width(const D2D1_RECT_F& r) { return r.right - r.left; }
inline float Height(const D2D1_RECT_F& r) { return r.bottom - r.top; }
