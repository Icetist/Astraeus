#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <memory>
#include <string>

#include "anim.h"
#include "config.h"
#include "model.h"
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
    const Theme& Shell() const { return shellNow; } // current palette, tray menu matches it
    void Toggle();
    void Peek(const std::wstring& title, const std::wstring& detail, int icon = 0); // brief notification drop, 0 = timer icon
    void SetFull(bool on); // fullscreen app present, stay out of the way
    void SyncHide();
    void OnSecond();

    bool Busy() const { return opened || peeking || dragging; } // user is looking, don't restart now
    bool NeedsFrame() const { return needsFrame; } // main loop polls this to keep rendering
    HANDLE FrameWait() const { return frameWait; } // swapchain waitable, paces to vsync
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
        if (opened || dragging || peeking || spring.value > 0.5f) needsFrame = true; // skip frames when fully closed
    }

    bool EnsureDev();
    void FreeDev(); // drop gpu objects when idle
    void SyncCover(); // reload album art on track change
    ComPtr<ID2D1Bitmap> Blur(ID2D1Bitmap1* source);
    ComPtr<ID2D1Geometry> Shape(float h);
    ComPtr<ID2D1Geometry> BuildShape(float h) const;
    void DrawBody(float h, float dt, double now);
    void DrawPeek(float h);
    void DrawRail(const Theme& t);
    void SyncWall();
    void Perf(double drawSec);
    void SetShell(const Theme& t);
    void SetMedia(const Theme& t);
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
    anim::Spring spring;
    bool opened = false, dragging = false, peeking = false;
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
    float shapeH = -1; // -1 forces first rebuild

    Theme shellFrom, shellTo, shellNow, mediaFrom, mediaTo, mediaNow; // from/to lerp into now
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
