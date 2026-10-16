#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <algorithm>
#include <memory>

#include "lyricview.h"
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
    bool Leaving() const { return leaving; } // on its way out: the panel can come back under it
    HANDLE Wait() const { return leaving && leaveTick ? leaveTick : frameWait; } // vsync waitable, the main loop sleeps on it; a timer while leaving
    void Frame(const Theme& media);

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool MakeDev(UINT pxW, UINT pxH);
    struct Glass; // live desktop blur behind the stage, glass appearance only
    Glass* glass = nullptr;
    bool OpenGlass(UINT pxW, UINT pxH);
    void FreeGlass();
    void Leave(); // the user is done: shrink back into the bezel, then go
    bool GlassLeave(float seconds, float cx, float cy); // the same, run by windows' compositor in the glass look
    bool leaving = false;
    double leaveAt = 0;
    HANDLE leaveTick = nullptr; // paces the loop while the compositor plays the exit: nothing is presented then
    void SyncCover();
    void SyncTheme(const Theme& media, float dt);
    void Backdrop(double now);
    void Clock(float y, float k);
    void Player(float show, float lyr);
    void SyncLyrics(double now);
    D2D1_RECT_F LyricRect() const; // the column the lyrics scroll in, right of the cover
    float LyricSize() const { return std::clamp(H * 0.05f, 24.0f, 64.0f); }
    void Seek(const D2D1_RECT_F& r, float show);

    Model& model;
    HWND hwnd = nullptr;
    float scale = 1, W = 0, H = 0; // dips
    bool done = false;
    double lastFrame = 0, lastMove = -10, openedAt = 0;
    POINT lastPt = {-1, -1};
    float preview = -1; // seek drag preview
    bool cursorGone = false; // hidden after the mouse rests

    LyricView lyrics; // only for songs from music apps and sites, when lrclib has them synced
    std::wstring lyrKey;
    std::shared_ptr<const sys::Lyrics> lyrDoc;
    double lyrAsked = -1, lyrPoll = 0, lyrKeyAt = 0;
    float lyrMix = 0; // 0 the classic layout, 1 the cover on the left with the lyrics beside it

    float left = 0, right = 0, slow = 0, beat = 0; // eased channel levels, running average, beat pulse
    float frameDt = 1 / 60.0f;
    double lastBeat = 0;
    static constexpr int kBlobs = 6;
    Swell swell; // soft light rising along the bottom of the screen

    int mode = 0; // the panel's appearance when the stage opened: 0 dark, 1 light, 2 glass, 3 custom
    bool LightRoom() const { return mode == 1 || (mode == 3 && theme::Luma(look.bg) >= 0.18f); }
    double nudgeAt = -10; // when [ or ] last moved the lyrics, for the little readout
    float shade = 0; // glass over a bright desktop: extra dark tint so white text reads
    Theme look, lookFrom, lookTo; // that appearance in the song's hue, crossfading from the last song's
    float lookT = 1;
    bool lookSet = false;
    bool artSeeded = false; // colors come from this song's own cover, the panel isn't drawing to update them
    Seed artSeed;
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
