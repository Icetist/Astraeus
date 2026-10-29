#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <memory>
#include <string>
#include <vector>

#include "anim.h"
#include "hud.h"
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
    void KeyOpen(); // from the shortcut: open on the screen with the mouse and take the keyboard, or close
    void Peek(const std::wstring& title, const std::wstring& detail, int icon = 0); // brief notification drop, 0 = timer icon
    void SetFull(bool on); // fullscreen app present, stay out of the way
    void SyncHide();
    void OnSecond();
    void ShowSettings(); // the tray's settings item: the panel opens with its settings sheet up
    void Wake(); // launched again while running: whatever kept it hidden is dropped and it shows itself
    void Greet(); // at sign-in: the home page drops down and stays until the mouse moves
    void PlayFull(); // the fullscreen player for the playing song, asked for from outside (halobar's cover), panel open or not
    void Reconfig(); // astraeus.ini changed on disk: sizes and corners follow without a restart

    bool Busy() const { return opened || peeking || dragging || island; } // user is looking, don't restart now
    bool NeedsFrame() const { return needsFrame || stage; } // main loop polls this to keep rendering
    HANDLE FrameWait() const { return stage ? stage->Wait() : frameWait; } // swapchain waitable, paces to vsync
    void Frame();

private:
    enum : UINT_PTR { kHoverTimer = 1, kCloseTimer, kReleaseTimer, kPeekTimer, kDevTimer, kFollowTimer, kTrimTimer = 8, kFullTimer, kGreetTimer, kDeskTimer, kAdoptTimer }; // all one-shot; 7 is the model's
    static constexpr UINT kAdoptMsg = WM_APP + 60; // a hover strip on another screen asks the panel to come over
    static constexpr float kStrip = 3; // idle bezel height in dips, just enough to catch the mouse
    static constexpr UINT kShelfMsg = WM_APP + 61; // the shelf page asked for a drag out, run after the frame: it's modal
    static constexpr int kShelfPage = Model::kPgShelf; // pages go by identity; model.pages holds the rail's order
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    struct DropSink; // the island as an ole drop target, feeding the shelf
    DropSink* drop = nullptr;
    bool dragSelf = false; // our own drag out is under way: dropping it back on the panel does nothing
    void StartShelf(); // registers the drop target, ole is up on this thread
    void StopShelf();
    bool DropEnter(); // something droppable came over the island: the panel opens on the shelf
    void DropLeave();
    void DropDone(IDataObject* data);
    void ShelfOut(); // the modal drag of one shelf item to wherever the user lets go
    HPOWERNOTIFY acNote = nullptr, batNote = nullptr; // plugged in or out, battery percent: laptops only
    int acNow = -1; // -1 until windows' first notice, which only says how things are
    void WatchPower(); // laptops: register for the two power notices
    void OnPower(const POWERBROADCAST_SETTING& s);
    static constexpr float kRailW = 72; // the page rail's width in dips
    static constexpr float kBubble = 30, kBubbleGap = 6; // the live activity bubble beside the pill
    bool Bubble() const; // a timer, stopwatch or focus session sits beside the resting pill
    D2D1_RECT_F BubbleBox() const; // where, in window dips
    bool bubbleNow = false; // the window region was cut with the bubble
    void DrawBubble();
    std::wstring PillSig() const; // what a still pill shows, to redraw only when that changes
    std::wstring pillSig;
    bool pageSet = false; // whoever opens the panel already chose its page
    ULONGLONG wheelAt = 0; // last page step from the wheel, a swipe moves one page
    float railWheel = 0; // wheel notches over the rail or pill, a page per whole notch
    void WheelPages(float notches); // the rail or the closed pill: next or previous page
    int zoomNow = 100; // the ui scale the window is built for
    float Zoom() const { return model.uiScale / 100.0f; } // the scale setting is just a bigger dip: everything follows, hit tests too
    sys::VolumeWatch volWatch; // these three only while the hud option is on
    sys::BrightWatch brightWatch;
    sys::VolumeKeys volKeys;
    bool hudNow = false;
    void SyncHud();
    void RetryHud();
    void ShowHud(int icon, float level, const wchar_t* title); // the volume or brightness pill, a short drop with a bar
    void OnVolumeKey(UINT vk);
    float peekLevel = -1; // a hud pill's bar 0..1, -1 a plain notice
    ComPtr<ID2D1Bitmap1> coverPrev; // the last cover, for the art's flip

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
    void FreeDev();
    void TrimDev(); // keep the device, drop its caches // drop gpu objects when idle
    bool RenewDev(); // a resting pill: swapped onto a fresh device its own size, what the open panel grew in the driver goes with the old one
    void ReleaseLater(); // schedule FreeDev if the panel stays shut
    UINT ambientSync = 2; // present interval for ambient-only motion, ~30 fps at any refresh rate
    UINT pillSync = 4; // vsyncs per frame for the resting pill's bars, ~15 fps
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
    uint64_t trimKey = 0; // the resting pill the last cache trim was scheduled for
    bool devWide = false; // this device has drawn more than the pill: once a pill rests again it moves to a fresh one
    float IslandSpan() const { return Side() ? kIslandSide : kIslandW; } // pill length along its edge
    void ShowIsland(); // small pill instead of the panel, over maximized apps
    bool AppBelow() const; // an app window lies where the panel would drop
    bool Stays() const; // the resting pill holds still: by itself, or because the desktop is what's under it
    void SyncStay(); // the pill comes back by itself, or goes once nothing holds it up
    void WatchDesk(); // pill on the desktop: the window hooks go on with the option and off without it
    void UnwatchDesk();
    void OnDeskTimer(); // the windows have settled: is the desktop what's under the island now
    void HookFront(HWND front); // the front app's own moves: maximize, snap and win+arrows change no focus
    static void CALLBACK DeskEvent(HWINEVENTHOOK, DWORD ev, HWND w, LONG obj, LONG child, DWORD, DWORD);
    HWINEVENTHOOK deskHooks[3] = {}; // focus changed, a move or resize ended, a minimize began or ended
    HWINEVENTHOOK frontHook = nullptr; // location changes, from the front window's thread only
    DWORD frontThread = 0;
    bool deskOn = false; // the hooks are in: the option was on last second
    bool deskView = false; // nothing of an app's under the island, or the desktop has the focus
    RECT Footprint() const; // that area on screen, in pixels
    void DrawRail(const Theme& t);
    void SyncWall();
    void Perf(double drawSec);
    void Log(const wchar_t* what) const; // with perflog on: one line about where the panel is, for multi screen reports
    bool FullOn(HMONITOR m) const; // another app fills that screen
    HMONITOR Refuge() const; // every screen mode: where the pill goes when a game takes its screen
    void Hop(HMONITOR to); // move there now
    void Retract(); // panel and hover state dropped instantly
    static bool Overlay(HWND w); // screen sized but never a fullscreen app: an overlay, a click-through layer, a tool window, halobar
    int fullChecks = 0; // the shell said fullscreen: looks left to confirm it, on a short timer
    void OnFullTimer(); // one of those looks
    void OnGreetTimer(); // drops the greeting, then watches the cursor
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
    bool GlassResize(UINT w, UINT h); // the composition surface and visuals to a new pixel size, in place
    bool GlassRenew(ID2D1Device* dev); // the same visuals and blur, a new surface on dev to draw into; null lets go of the old one
    void FitSurfaces(); // a pill alone needs a short strip, not the whole panel's surfaces
    UINT surfW = 0, surfH = 0; // the surfaces' size now, pixels
    void GlassBlur(float amount); // 0 clear .. 1 fully frosted
    void SampleBehind(); // glass: how bright the screen under the panel is
    float glassShade = 0; // 0 .. 1, deepens the glass tint over bright backgrounds so white text stays readable
    float glassShadeNow = 0; // what's drawn: eases to glassShade, so the tint follows the page behind without a jump
    int shadeTick = 0; // seconds toward the next look behind the glass
    void SampleShade(); // glass, while showing: what's behind it now, every half minute
    void SetMedia(const Theme& t);
    void OpenStage(); // fullscreen ambient player, replaces the panel until it's left
    std::unique_ptr<Stage> stage;
    std::unique_ptr<Stage> twin; // two screens: the other half of the pair, paced by the first
    HMONITOR stageHome = nullptr; // the screen fullscreen was opened on
    int twinSplit = -1; // the two screen setting the pair was last built for
    bool twinFailed = false; // the second window couldn't be made: not tried again every frame
    void SyncTwin(); // the second half comes, goes or moves to follow the setting
    HMONITOR TwinScreen() const; // the nearest other screen; null with one screen
    void StageFrame(); // both halves: the second draws when its own screen is ready for it
    bool stageBack = false; // the panel already reopened under a leaving stage
    bool greeting = false; // the sign-in greeting is up: the first move of the mouse puts it away
    int greetTries = 0;
    POINT greetAt{};
    bool stageHalo = false; // opened from halobar's cover: leaving it opens halobar's own player again
    bool stageAway = false; // opened from outside: leaving it goes straight back to the desktop, no panel
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
    bool keys = false; // opened from the shortcut: the panel holds the keyboard until it closes
    HWND keysFrom = nullptr; // where the keyboard goes back to
    bool pillRgn = false; // the island's window region is on: the window is panel-wide, only the pill is live
    RECT pillRect{}; // that region, to skip setting it again unchanged
    bool eco = false; // process in efficiency mode
    bool locked = false, screenOff = false; // nobody can see the pill: its bars stop until the screen is back
    HPOWERNOTIFY displayNote = nullptr; // display on/off notices
    bool Unseen() const { return locked || screenOff; }

    static constexpr float kPeekH = 66; // peek banner height in dips
    static constexpr float kPeekSide = 300; // peek reach on a side edge, room for its text
    static constexpr float kIslandH = 34, kIslandW = 260; // island pill size in dips, width includes the shoulders
    static constexpr float kIslandSide = 240; // on a side edge the song runs along the pill like a book spine
    bool islandHot = false; // mouse over the island: show the expand hint
    bool stayWasAlways = false; // the island was set to always last second, to notice it being switched off
    float islandBars[4] = {}; // mini visualizer levels, eased per frame
    static constexpr UINT kIslandMs = 500; // dwell before the island shows over an app window
    anim::Spring spring;
    bool opened = false, dragging = false, peeking = false;
    bool island = false; // island pill is up (or dropping)
    bool islandLook = false; // draw the narrow island shape, until it has fully retracted
    bool grow = false; // panel is expanding out of the island, width morphs
    bool islandNext = false; // the pending hover should raise the island, not the panel
    float winW = 0; // current window length along the edge in dips
    int edgeNow = 0, screensNow = 0; // placement the window is built for
    std::wstring screenNow; // the picked screen the window is built for
    bool PlaceStale() const { return model.Edge() != edgeNow || model.screens != screensNow || model.screen != screenNow; }
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
    int framesSeen = 0, framesLate = 0; // the governor's window of animation frames
    UINT fastSync = 1; // vsyncs per animation frame: 1 normally, more on a machine that can't keep up
    bool lastFast = false;
    double syncRetry = 0; // when to try full rate again
    void Govern(bool fast, double gap); // steps animation down to a rate the machine holds, and back up
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
    bool followCall = false; // a resting pill was called from that screen: once there, it answers like a hover
    HMONITOR adoptTo = nullptr; // a strip on another screen was touched while the pill rests here: moves after a moment's dwell
    void Answer(); // just moved to the screen that called: the panel or the pill, as a hover would
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
    ComPtr<IDCompositionDesktopDevice> dcomp; // made without a rendering device: a swapchain from a new device can go in its visual
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    HANDLE frameWait = nullptr;
    std::unique_ptr<Canvas> canvas;

    uint64_t coverId = 0;
    ComPtr<IWICBitmapSource> coverSrc;
    ComPtr<ID2D1Bitmap1> cover;
    ComPtr<ID2D1Bitmap> coverBlur;
};
