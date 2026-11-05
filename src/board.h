#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <memory>
#include <string>
#include <vector>

#include "access.h"
#include "lyricview.h"
#include "model.h"
#include "pages.h"
#include "theme.h"
#include "ui.h"

// The fullscreen dashboard: one screen given over to tiles, over every window there. A screen switched back to the desktop
// has no window and no device at all. Its picture lives in one surface that keeps its pixels: only a tile whose content
// changed is painted again, the windows compositor redoes only that patch, and nothing runs while a game or video fills its screen.
class Board {
public:
    Board(Model& model, HMONITOR mon, const std::wstring& name);
    ~Board();
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

    bool Ok() const { return ctx != nullptr; }
    HMONITOR Screen() const { return screen; }
    const std::wstring& Name() const { return name; }
    const std::vector<Model::BoardTile>& Tiles() const { return tiles; }
    bool Needs() const { return (dirty || busy) && !covered && ctx; } // a frame is due
    bool Covered() const { return covered; } // a game or video has its screen
    bool Leaving() const { return leave; } // its button asked for the desktop back
    HANDLE Wait() const { return frameWait; }
    void Frame(const Theme& shell, const Theme& media);
    void Second(const Theme& shell, const Theme& media); // once a second: whatever it shows changed, or a fullscreen app came or went
    void TakeKeys(); // the keyboard, until esc or another window takes it back

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool MakeDev(UINT pxW, UINT pxH);
    void Fit(); // the grid's rows for this screen, and tiles that fit it
    D2D1_RECT_F CellRect(int x, int y, int w, int h) const; // grid cells to dips
    void Backdrop(); // the look behind the tiles
    void SyncCover();
    void SyncLyrics(double now);
    uint64_t Look(int kind) const; // a fingerprint of what a tile shows: the same as when it was painted means no paint
    void Poke(); // something needs a frame: soon, not this instant, however fast the mouse reports
    void Arm(double seconds);
    bool Paint(const RECT& px, double now, float dt); // one patch of the surface, in pixels: the backdrop and every tile touching it
    void Buttons(); // arrange, and back to the desktop
    D2D1_RECT_F ButtonsBox() const;
    RECT Pixels(const D2D1_RECT_F& r) const; // dips to whole pixels, outward, on the screen

    // the tiles, in board_tiles.cpp
    void DrawTile(const Model::BoardTile& tile, const D2D1_RECT_F& r, int index);
    void TileClock(const D2D1_RECT_F& r, float k);
    void TileCalendar(const D2D1_RECT_F& r, float k);
    void TileWeather(const D2D1_RECT_F& r, float k);
    void TileMedia(const D2D1_RECT_F& r, float k);
    void TileLyrics(const D2D1_RECT_F& r, float k);
    void TileGauge(const D2D1_RECT_F& r, float k, int kind);
    void TileNet(const D2D1_RECT_F& r, float k);
    void TileDisk(const D2D1_RECT_F& r, float k);
    void TileProcs(const D2D1_RECT_F& r, float k);
    void TileVolume(const D2D1_RECT_F& r, float k);
    void TileTodo(const D2D1_RECT_F& r, float k);
    void TileWork(const D2D1_RECT_F& r, float k);
    void TileShelf(const D2D1_RECT_F& r, float k);
    void TileTimer(const D2D1_RECT_F& r, float k);
    void TileServers(const D2D1_RECT_F& r, float k);
    void TileBattery(const D2D1_RECT_F& r, float k);
    uint64_t copiedId = 0; // the shelf card just copied, ticked for a moment
    double copiedAt = -10;

    // arranging, in board_edit.cpp
    void Edit(); // tiles as outlines to drag and resize
    void EditFrame();
    void Picker(const D2D1_RECT_F& bar); // the kinds not on this screen yet
    bool Free(int x, int y, int w, int h, int skip) const; // no other tile there, and inside the grid
    bool Place(int kind); // a new tile in the first room big enough
    void Save(); // this screen's tiles, kept
    D2D1_POINT_2F CellAt(D2D1_POINT_2F p) const; // the grid cell under a point, fractional

    static constexpr float kMargin = 14, kGap = 10; // around the grid and between its cells, dips
    Model& model;
    std::wstring name; // the monitor's own id: its tiles are kept under it
    HWND hwnd = nullptr;
    HMONITOR screen = nullptr;
    float scale = 1, W = 0, H = 0; // dips
    int rows = 7;
    float cellW = 0, cellH = 0; // one cell, without the gaps
    bool dirty = true, busy = false, covered = false, leave = false;
    bool armed = false, repaint = true; // a frame is booked; the whole surface is due
    bool wasEditing = false, buttons = false, hand = false, wasOnBox = false;
    bool keyMode = false; // the keyboard's here: focus moves over tiles, so they're painted whole
    HWND keysFrom = nullptr; // where the keyboard goes back to
    Access access; // what a screen reader sees
    D2D1_POINT_2F paintShift{}; // where the last patch sat in windows' atlas, dips
    std::vector<uint64_t> seen; // per tile, the fingerprint last painted
    std::vector<char> stale, moving; // per tile: changed since its paint; animating, so every frame paints it
    int hoverTile = -1, tick = 0, rest = 0;
    uint64_t shellKey = 0, mediaKey = 0;
    double lastFrame = 0, movedAt = -10;
    POINT lastPt = {-1, -1};
    Input input;
    Ui ui;
    Theme t, mt; // the shell's look, solid, and the song's

    bool editing = false, picking = false;
    std::vector<Model::BoardTile> tiles; // what this screen shows
    int grab = -1; // the tile being dragged or resized
    bool sizing = false;
    D2D1_POINT_2F grabAt{}; // where the drag began, in tile cells
    int toX = 0, toY = 0, toW = 0, toH = 0; // where it would land

    uint64_t coverId = ~0ull;
    ComPtr<ID2D1Bitmap1> cover;
    ComPtr<ID2D1Bitmap> coverBlur;
    LyricView lyrics;
    std::wstring lyrKey;
    std::shared_ptr<const sys::Lyrics> lyrDoc;
    double lyrAsked = -1, lyrPoll = 0, lyrKeyAt = 0;
    float lyrSize = 0, lyrWidth = 0;
    Hist gpuHist{60};
    float diskUsed = -1;
    double diskFree = 0;
    int diskTick = 0, wxTick = 0;
    int calMonth = 0; // months away from this one the calendar tile shows
    float level = 0; // the music's level, eased, for the media tile's glow
    Waveform wave; // the media tile's visualizer, in the look picked in settings
    Swell swell;

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<IDCompositionSurface> surface;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    HANDLE frameWait = nullptr;
    std::unique_ptr<Canvas> canvas;
};
