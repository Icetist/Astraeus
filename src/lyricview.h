#pragma once
#include <memory>
#include <vector>

#include "anim.h"
#include "gfx.h"
#include "lyrics.h"
#include "theme.h"
#include "ui.h"

// The fullscreen player's lyrics: the sung line lit word by word, the rest soft and out of focus, everything gliding up in a ripple.
// Each line's blur is baked once into a small texture, so a frame is only tinted copies and a few text draws.
class LyricView {
public:
    void Sync(ID2D1DeviceContext* ctx, const std::shared_ptr<const sys::Lyrics>& doc, float width, float size); // outside BeginDraw: lays the lines out
    void Prepare(ID2D1DeviceContext* ctx); // outside BeginDraw: bakes the blur of a few lines near the sung one, a handful per frame
    bool Has() const { return !rows.empty(); }
    bool Busy() const { return moving; } // lines still gliding: keep frames at full rate
    // Draws into r at song time t (seconds); returns the time of a line that was clicked, or -1.
    double Draw(ID2D1DeviceContext* ctx, Canvas& c, Ui& ui, const D2D1_RECT_F& r, double t, float alpha, const Theme& look, bool playing);
    void Waiting(Canvas& c, const D2D1_RECT_F& r, float alpha, const Theme& look, double now); // the next song's lyrics are on their way
    struct Look { // the full player's look by default; the lyrics half of a two screen pair sets its own
        float anchor = 0.30f; // the sung line sits this far down the column
        float shrink = 0.045f; // lines out of turn this much smaller
        float gap = 0.62f; // air between lines, a share of the type size
        float stiff = 150, stiffStep = 11, damp = 0.9f; // the glide: the farther a line, the later it lands
        float dim = 0.34f, dimLit = 0.42f; // lines out of turn, and the unsung words of the sung line
        float haze = 1; // how far out of focus the lines away from the sung one go
        float dimStep = 0.04f; // each line further out this much dimmer, up to four
    };
    void Style(const Look& l); // relays the sweep's colors if they change

private:
    struct Span { // one visual line of a wrapped lyric
        UINT32 first, len;
        float left, width, top, height;
    };
    struct Piece { // a block of wrapped text
        ComPtr<IDWriteTextLayout> lay;
        std::vector<Span> spans;
        float w = 0, h = 0, y = 0; // y: where it sits in the row
        UINT32 len = 0;
    };
    struct Row {
        const sys::LyricLine* src = nullptr;
        Piece lead;
        ComPtr<ID2D1Bitmap1> haze; // the line blurred, white, baked once
        float h = 0, w = 0;
        anim::Spring y;
        float lit = 0, hover = 0; // 1 while it's the sung line, fades as it passes
        float blur = 1; // out of focus by distance from the sung line, eased
    };

    bool Lay(Piece& pc, const std::wstring& text, IDWriteTextFormat* f, float lineH);
    float Sweep(const Row& row, double t) const; // characters sung so far
    void Ink(ID2D1DeviceContext* ctx, const Row& row, D2D1_POINT_2F at, ID2D1Brush* brush); // plain
    void Sweeping(ID2D1DeviceContext* ctx, Piece& pc, D2D1_POINT_2F at, float chars, float a, Color text, size_t& e);
    void Bake(ID2D1DeviceContext* ctx, Row& row);
    void Haze(ID2D1DeviceContext* ctx, const Row& row, D2D1_POINT_2F at, float scale, Color col);
    void Text(ID2D1DeviceContext* ctx, const Row& row, D2D1_POINT_2F at, float scale, Color col);
    void Lit(ID2D1DeviceContext* ctx, Row& row, D2D1_POINT_2F at, float scale, double t, const Theme& look, float a);
    void Dots(Canvas& c, D2D1_POINT_2F at, float progress, float appear, const Theme& look, float a, double now);

    std::shared_ptr<const sys::Lyrics> doc;
    std::vector<Row> rows;
    std::vector<float> tops; // scratch, reused each frame
    ComPtr<ID2D1SolidColorBrush> ink, sung, unsung;
    ComPtr<ID2D1GradientStopCollection> stops;
    std::vector<ComPtr<ID2D1LinearGradientBrush>> edges; // the sweep's soft edge, one per wrapped piece, reused
    Color stopsFor = {-1, -1, -1, -1};
    float width = 0, size = 0, pad = 0;
    float manual = 0; // wheel offset, eases back after a pause
    double manualAt = -10;
    float gap = 0; // interlude spacer height, eased
    int focus = 0; // the sung line, where baking starts
    bool placed = false, moving = false;
    Look style;
};
