#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <memory>
#include <string>
#include <vector>

#include "anim.h"
#include "config.h"
#include "model.h"
#include "stage.h"
#include "theme.h"
#include "ui.h"

class Drawer {
public:
    Drawer(const Config& cfg, Model& model);
    ~Drawer();
    Drawer(const Drawer&) = delete;
    Drawer& operator=(const Drawer&) = delete;

    bool Create(HINSTANCE instance);
    HWND Hwnd() const { return hwnd; }
    Theme Shell() const { // current palette for the tray menu and prompts: they're solid windows, so glass lends them its dark twin
        return shellNow.glass ? Theme::From(Seed{shellSeed.hue, shellSeed.chroma, 0}) : shellNow;
    }
    void Toggle();
    void Peek(const std::wstring& title, const std::wstring& detail, int icon = 0); // brief notification drop, 0 = timer icon
    void SetFull(bool on); // fullscreen app present, stay out of the way
    void SyncHide();
    void OnSecond();

    bool Busy() const { return opened || peeking || dragging || island; } // user is looking, don't restart now
    bool NeedsFrame() const { return needsFrame || stage; } // main loop polls this to keep rendering
    HANDLE FrameWait() const { return stage ? stage->Wait() : frameWait; } // swapchain waitable, paces to vsync
    void Frame();

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Place();
    void Resize(float hDip, bool topmost); // window only as tall as visible shape
    D2D1_POINT_2F ToDip(LPARAM lp) const; // mouse px to dip coords
    void TrackLeave();
    bool MouseIn() const;

    void Open();
    void Close();
    void SetPage(int page);
    void StartDrag(float y);
    void MoveDrag(float y);
    void EndDrag();
    float Rubber(float raw) const; // resist drag past full height
    void Redraw() {
        if (opened || dragging || peeking || island || spring.value > 0.5f) needsFrame = true; // skip frames when fully closed
    }

    bool EnsureDev();
    void FreeDev(); // drop gpu objects when idle
    void ReleaseLater(); // schedule FreeDev if the panel stays shut
    UINT ambientSync = 2; // present interval for ambient-only motion, ~30 fps at any refresh rate
    double hz = 60; // screen refresh, paces glass frames
    void SyncCover(); // reload album art on track change
    ComPtr<ID2D1Bitmap> Blur(ID2D1Bitmap1* source);
    ComPtr<ID2D1Geometry> Shape(float h, float w); // w = length along the edge incl. shoulders, centered
    bool Side() const { return edgeNow != 0; } // docked to the left or right edge
    float CW() const { return Side() ? kSideW : W - 2 * cfg.fillet; } // content width
    float CH() const { return Side() ? sideH : H; } // content height: on a side edge a tall sidebar
    float Depth() const { return Side() ? kSideW : H; } // how far the open panel reaches from its edge
    float Span() const { return Side() ? sideH + 2 * cfg.fillet : W; } // its length along the edge, shoulders included
    void DrawRailTop(const Theme& t); // sidebar: page icons in a row along the top
    static constexpr float kSideW = 400, kSideMaxH = 860, kTopRail = 58; // sidebar size, top rail height
    float sideH = kSideMaxH; // fitted to the screen in Place
    float DepthOf(D2D1_POINT_2F p) const; // window point -> distance from the docked edge
    D2D1_MATRIX_3X2_F EdgeXform() const; // the shape is built for the top edge, this turns it to the docked one
    void ApplyPlace(); // edge or screens setting changed: re-dock, rebuild hover strips
    void BuildStrips(); // invisible hover strips on the other screens
    void FreeStrips();
    static LRESULT CALLBACK StripProc(HWND, UINT, WPARAM, LPARAM);
    ComPtr<ID2D1Geometry> BuildShape(float h, float w) const;
    void DrawBody(float h, float dt, double now);
    void DrawPeek(float h);
    void DrawHint(float h); // the page's tip, just outside the panel
    static constexpr float kHintRoom = 40; // extra window depth while a tip shows
    const wchar_t* hintText = nullptr;
    D2D1_RECT_F hintRect{}; // window coords
    float hintFade = 0;
    bool DrawIsland(float h, float dt, double now); // true while its visualizer is moving
    bool DrawIslandSide(float h, float a); // the side edges' vertical pill: art, the song turned sideways, bars
    bool StepIslandBars(float dt, double now); // mini visualizer levels, true while moving
    void PaintShape(ID2D1Geometry* shape, float h); // the panel or pill body: fill, smoked glass, sheen, rim
    D2D1_RECT_F Visible() const; // the corner of the surface the window shows
    bool PillLayer(float h, float shapeWide); // a resting pill's still parts drawn once into a bitmap; false: draw it the normal way
    uint64_t PillKey(float h) const; // everything the still parts depend on
    int pillPass = 0; // 0 the whole pill, 1 only its still parts (into the layer), 2 only the moving bars
    ComPtr<ID2D1Bitmap1> pillLayer; // the resting pill without its bars
    uint64_t pillKey = 0;
    float IslandSpan() const { return Side() ? kIslandSide : kIslandW; } // pill length along its edge
    void ShowIsland(); // small pill instead of the panel, over maximized apps
    bool MaximizedBelow() const; // a maximized app sits under the hot zone
    bool TiledEdge(const RECT& work) const; // tiled windows cover the whole docked edge between them
    void DrawRail(const Theme& t);
    void SyncWall();
    void Perf(double drawSec);
    void SetShell(const Theme& t);
    Theme Make(Seed s) const; // palette for a seed in the current appearance
    void SyncLook(); // appearance changed: crossfade palettes, swap presentation for glass
    void SyncAccent(); // accent source or custom color changed
    void SyncTyping(); // hex field focused: take the keyboard, give it back after
    struct Glass; // live blur behind the panel, only while the glass appearance is on
    Glass* glass = nullptr;
    bool OpenGlass(const DXGI_SWAP_CHAIN_DESC1& desc); // the frame goes straight into composition, in step with the blur
    bool GlassDev(const DXGI_SWAP_CHAIN_DESC1& desc); // glass device: a plain target bitmap and a frame timer, no swapchain
    void GlassPresent(); // hand this frame to composition, it lands together with the blur outline
    void GlassMask(ID2D1Geometry* shape); // cut the blur to this frame's shape
    void FreeGlass();
    void GlassBlur(float amount); // 0 clear .. 1 fully frosted
    void SampleBehind(); // glass: how bright the screen under the panel is
    float glassShade = 0; // 0 .. 1, deepens the glass tint over bright backgrounds so white text stays readable
    void SetMedia(const Theme& t);
    void OpenStage(); // fullscreen ambient player, replaces the panel until it's left
    std::unique_ptr<Stage> stage;
    bool stageBack = false; // the panel already reopened under a leaving stage
    bool StepTheme(float dt); // crossfade palettes, true while moving

    const Config& cfg;
    Model& model;
    HWND hwnd = nullptr;
    float scale = 1; // dpi / 96
    RECT monitor = {};
    float W = 0, H = 0; // panel size in dips
    const float overshoot = 46; // extra room for spring bounce below H
    float winH = 0; // current window height in dips
    bool hidden = false, fullApp = false, tracking = false, armed = false;
    bool eco = false; // process in efficiency mode

    static constexpr float kPeekH = 66; // peek banner height in dips
    static constexpr float kPeekSide = 300; // peek reach on a side edge, room for its text
    static constexpr float kIslandH = 34, kIslandW = 260; // island pill size in dips, width includes the shoulders
    static constexpr float kIslandSide = 240; // on a side edge the song runs along the pill like a book spine
    bool islandHot = false; // mouse over the island: show the expand hint
    bool stayWasAlways = false; // the island was set to always last second, to notice it being switched off
    float islandBars[4] = {}; // mini visualizer levels, eased per frame
    static constexpr UINT kIslandMs = 500; // dwell before the island shows over a maximized app
    anim::Spring spring;
    bool opened = false, dragging = false, peeking = false;
    bool island = false; // island pill is up (or dropping)
    bool islandLook = false; // draw the narrow island shape, until it has fully retracted
    bool grow = false; // panel is expanding out of the island, width morphs
    bool islandNext = false; // the pending hover should raise the island, not the panel
    float winW = 0; // current window length along the edge in dips
    int edgeNow = 0, screensNow = 0; // placement the window is built for
    std::wstring screenNow; // the picked screen the window is built for
    bool PlaceStale() const { return model.edge != edgeNow || model.screens != screensNow || model.screen != screenNow; }
    HMONITOR mon = nullptr; // screen we live on, null = primary
    std::vector<HWND> strips; // hover strips on other screens, all-screens mode only
    D2D1_MATRIX_3X2_F shapeXform = D2D1::Matrix3x2F::Identity(); // this frame's shape placement, the glass mask reuses it
    std::wstring peekTitle, peekDetail;
    int peekIcon = 0;
    float dragOff = 0, dragY0 = 0, dragY = 0, dragVel = 0, pressTo = 0; // drag state, vel for fling
    double dragT = 0;

    Input input;
    Ui ui;
    bool hand = false;
    int page = 0, prevPage = 0;
    float pageT = 1; // page slide progress 0..1
    float railTop = 0, railBot = 0;

    double lastFrame = 0;
    bool needsFrame = false;
    double perfT0 = 0, perfDraw = 0;
    int perfN = 0;
    int perfWhy[6] = {}; // counts per redraw reason
    ComPtr<ID2D1Geometry> shapeC;
    float shapeH = -1, shapeW = -1; // -1 forces first rebuild

    Theme shellFrom, shellTo, shellNow, mediaFrom, mediaTo, mediaNow; // from/to lerp into now
    Seed shellSeed, mediaSeed; // what the palettes were built from, rebuilt on appearance change
    int lookNow = 0;
    HMONITOR followTo = nullptr; // every screen mode: the screen the open panel is moving to
    int railTip = -1, railTipShown = -1; // rail icon under the mouse, and the one whose label is showing or fading
    D2D1_POINT_2F railTipAt{};
    void DrawRailTip(const Theme& t);
    int devTries = 0; // failed device builds in a row, retried on a short timer
    int bgNow = 0; // custom look background the themes were built with
    int accentModeNow = 0, accentHexNow = 0;
    bool typing = false;
    HWND typingFrom = nullptr; // window that had the keyboard before the hex field took it
    float shellT = 1, mediaT = 1;
    std::wstring wallKey;

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<IDXGISwapChain2> swap;
    ComPtr<ID2D1Bitmap1> backBuf;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    HANDLE frameWait = nullptr;
    std::unique_ptr<Canvas> canvas;

    uint64_t coverId = 0;
    ComPtr<IWICBitmapSource> coverSrc;
    ComPtr<ID2D1Bitmap1> cover;
    ComPtr<ID2D1Bitmap> coverBlur;
};
