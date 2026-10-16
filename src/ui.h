#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "theme.h"

struct Input {
    D2D1_POINT_2F mouse = {-1e6f, -1e6f}; // far offscreen = no hover
    bool down = false;
    bool pressed = false; // edge flags, true for one frame only
    bool released = false;
    D2D1_POINT_2F pressPos = {-1e6f, -1e6f}; // where the press began, for capture test
    float wheel = 0; // notches, consumed by first widget under mouse
    bool rclick = false; // right button released this frame
};

class Ui {
public:
    struct Hit {
        bool hover = false;
        bool down = false;
        bool clicked = false;
    };

    void Begin(Canvas& canvas, const Input& input, float dt, double now);
    void End();

    Canvas& C() { return *canvas; }
    const Input& In() const { return in; }
    float Dt() const { return dt; }
    double Now() const { return now; }

    bool Busy() const { return animating; } // true = schedule next frame now
    bool Ambient() const { return ambient; } // slow anims, can render at lower rate
    void KeepAlive() { ambient = true; }
    void Wake() { animating = true; }
    bool Hand() const { return hand; } // show hand cursor
    bool Captured() const { return active != 0; } // a widget owns the mouse
    bool IsActive(uint32_t id) const { return active == id; }
    void Block(bool on) { blocked = on; } // ignore input, e.g. page under an overlay
    bool RClick(const D2D1_RECT_F& r) const { return !blocked && in.rclick && Inside(r, in.mouse); }
    bool FreePress() const { return in.pressed && active == 0 && !claimed; } // press on empty space, e.g. close panel

    static uint32_t Id(const char* s, int index = 0); // hashed string id, stable across frames

    float Smooth(uint32_t id, float target, float rate = 16); // exp ease toward target, snaps on first use
    float SmoothFrom(uint32_t id, float target, float initial, float rate = 16);
    float Soft(uint32_t id, float target); // starts at 0, flags ambient not busy
    float Env(uint32_t id, float target); // fast attack, slow decay (audio levels)
    Hit Button(uint32_t id, const D2D1_RECT_F& r);
    bool Slider(uint32_t id, const D2D1_RECT_F& r, float& value, bool vertical);
    float Wheel(const D2D1_RECT_F& r);
    bool Hold(uint32_t id, const D2D1_RECT_F& r, float seconds, float& progress);
    void Forget();

    uint32_t HitAt(D2D1_POINT_2F p) const;
    uint32_t Hovered() const { return lastHover; }

private:
    void AddHit(uint32_t id, const D2D1_RECT_F& r);

    std::vector<std::pair<uint32_t, D2D1_RECT_F>> hits, nextHits; // last frame / being built
    uint32_t lastHover = 0;
    D2D1_MATRIX_3X2_F frameXf = D2D1::IdentityMatrix();
    Canvas* canvas = nullptr;
    Input in;
    float dt = 0;
    double now = 0;
    bool animating = false, ambient = false, hand = false, claimed = false, blocked = false;
    uint32_t active = 0;
    std::unordered_map<uint32_t, float> values; // per-id anim state, survives frames
    std::unordered_map<uint32_t, double> holdT; // press start time, -1 once fired
};

namespace w {
void Card(Ui& ui, const Theme& t, const D2D1_RECT_F& r, float radius = 22);
bool IconBtn(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, float size, int glyph, float iconPx,
                bool enabled = true, bool lit = false);
bool PlayBtn(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, float size, bool playing, bool enabled);
bool Tile(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, const wchar_t* label,
          const std::wstring& detail, bool on, bool enabled = true);
bool Pill(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, float& value, bool* iconTap, bool off = false); // off: muted, the fill greys out
void Gauge(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, float radius, float thick, float value);
void Spark(Canvas& c, const D2D1_RECT_F& r, const std::vector<float>& values, uint64_t version, float maxV,
               Color line, bool flip = false);
bool Chip(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, bool selected);
bool Switch(Ui& ui, const Theme& t, uint32_t id, D2D1_POINT_2F c, bool on, bool enabled = true); // 46x26 toggle
std::wstring Pct(float v);
}
