#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <memory>

#include "model.h"
#include "theme.h"
#include "pages.h"
#include "ui.h"

// Fullscreen ambient player: the album art big over drifting audio-reactive light, controls that fade out when the mouse rests.
// Built when opened from the media page and torn down on exit, so it costs nothing the rest of the time.
class Stage {
public:
    Stage(Model& model, HMONITOR mon);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    bool Ok() const { return ctx != nullptr; }
    bool Done() const { return done; } // user left, owner drops it
    HANDLE Wait() const { return frameWait; } // vsync waitable, the main loop sleeps on it
    void Frame(const Theme& media);

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool MakeDev(UINT pxW, UINT pxH);
    struct Glass; // live desktop blur behind the stage, glass appearance only
    Glass* glass = nullptr;
    bool OpenGlass(UINT pxW, UINT pxH);
    void FreeGlass();
    void SyncCover();
    void SyncTheme(const Theme& media);
    void Backdrop(double now);
    void Clock(float y, float k);
    void Player(float show);
    void Seek(const D2D1_RECT_F& r, float show);

    Model& model;
    HWND hwnd = nullptr;
    float scale = 1, W = 0, H = 0; // dips
    bool done = false;
    double lastFrame = 0, lastMove = -10, openedAt = 0;
    POINT lastPt = {-1, -1};
    float preview = -1; // seek drag preview

    float left = 0, right = 0, slow = 0, beat = 0; // eased channel levels, running average, beat pulse
    float frameDt = 1 / 60.0f;
    double lastBeat = 0;
    static constexpr int kBlobs = 6;
    Swell swell; // soft light rising along the bottom of the screen

    int mode = 0; // the panel's appearance when the stage opened: 0 dark, 1 light, 2 glass
    Theme look; // that appearance in the media's hue
    Color lookKey = {-1, -1, -1, -1};
    Input input;
    Ui ui;

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<IDXGISwapChain2> swap;
    ComPtr<ID2D1Bitmap1> backBuf;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    HANDLE frameWait = nullptr;
    std::unique_ptr<Canvas> canvas;

    uint64_t coverId = ~0ull;
    ComPtr<ID2D1Bitmap1> cover;
    ComPtr<ID2D1Bitmap> coverBlur;
};
