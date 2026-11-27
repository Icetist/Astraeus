#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <mfmediaengine.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "access.h"
#include "clipedit.h"
#include "model.h"
#include "sys_audiofx.h"
#include "sys_clips.h"
#include "sys_voice.h"
#include "theme.h"
#include "ui.h"

std::wstring StudioClock(double s, bool tenths); // 1:05, or 1:05.3

// The clip studio: one clip big over a quiet room, played with its sound by windows' own media engine on the gpu, with a
// timeline to trim it, undo and revert, and save over it or beside it. Built when a clip is opened and torn down when it's
// closed, so it costs nothing the rest of the time; while open it draws only while something moves or the clip plays.
class ClipStudio {
public:
    ClipStudio(Model& model, HMONITOR mon, const std::wstring& path);
    ~ClipStudio();
    ClipStudio(const ClipStudio&) = delete;
    ClipStudio& operator=(const ClipStudio&) = delete;

    bool Ok() const { return ctx != nullptr && engine != nullptr; }
    bool Done() const { return done; }
    bool Needs() const { return dirty || playing || leaving; } // the main loop waits on it only then
    HANDLE Wait() const { return leaving && leaveTick ? leaveTick : frameWait; }
    void Frame(const Theme& t);

private:
    using Cut = clipedit::Edit;
    struct Work { // what the worker threads hand back, under mu
        std::mutex mu;
        std::vector<sys::clips::Picture> strip;
        bool stripReady = false;
        bool done = false, ok = false; // a save or rename finished
        int kind = 0; // 1 saved over, 2 saved a copy, 3 renamed, 4 copied small
        std::wstring text, newPath;
        std::atomic<int> pct{-1}; // a clip being made: how far along
        std::shared_ptr<const sys::audiofx::Pcm> sound, song; // read off the ui thread, handed over here
        bool soundDone = false, songDone = false;
        std::vector<double> beats;
        std::wstring songWhy, songPath;
        std::wstring mixPath; // a mix written, and which edit it's for
        int mixVer = 0;
        bool mixDone = false;
    };

    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool MakeDev(UINT pxW, UINT pxH);
    bool MakeEngine();
    void Load();
    void Release(); // the file let go of: before it's replaced, renamed or binned
    void OnEngine(DWORD event);
    void TakeFrame();
    void Strip(); // pictures along the timeline, worked out off the ui thread
    void Leave();
    void Poke() { dirty = true; }

    void DrawTop(const Theme& t, float a);
    void DrawVideo(const D2D1_RECT_F& r, float a);
    void DrawBar(const Theme& t, float a);
    void Timeline(const Theme& t, const D2D1_RECT_F& r);
    void Ruler(const Theme& t, const D2D1_RECT_F& r); // the times along the top of the timeline
    void Bubble(const Theme& t, float x, float y, const std::wstring& text); // a time held up over the timeline
    bool Tool(const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* label, const wchar_t* tip, bool primary, bool live);
    bool IconTool(const Theme& t, const char* id, D2D1_POINT_2F c, int icon, const wchar_t* tip, bool live); // icon: a glyph, or one of the drawn ones
    void Tip(const D2D1_RECT_F& r, const wchar_t* text); // hovered: this explains it, after a moment
    void DrawTip(const Theme& t, double now);
    void DrawToast(const Theme& t); // how the last thing went, over the bar
    void AskLeave(const wchar_t* again); // an unsaved trim asks once

    // the effects panel and what it drives, in clipstudio_panel.cpp
    void DrawPanel(const Theme& t, const D2D1_RECT_F& r, float a);
    void PanelSpeed(const Theme& t, const D2D1_RECT_F& r);
    void PanelFrame(const Theme& t, const D2D1_RECT_F& r);
    void PanelSound(const Theme& t, const D2D1_RECT_F& r);
    void PanelMusic(const Theme& t, const D2D1_RECT_F& r);
    float PanelButton(const Theme& t, const char* id, float x, float y, float w, const wchar_t* label, const wchar_t* tip, bool primary, bool live, bool& hit); // returns the next row's top
    void Change(); // a step undo can go back to, and the mix made again if the sound changed
    void MixLater(); // the sound changed: mixed again once the edits settle
    void MixNow(); // off the ui thread
    void SyncMix(); // the mix plays along with the picture, in step
    void LoadSound(); // the clip's own sound, for the mix and for finding quiet parts
    void PickSong();
    void CutQuiet();
    void OnBeat(); // splits and the ends moved onto the song's nearest beats
    void VoiceToggle();
    void SplitHere();
    void RemoveHere();
    bool Framing() const { return tab == kTabFrame && clipedit::Cropping(cut); } // the whole picture shows, with the frame to drag
    float quietAmt = 0.5f; // how readily a stretch counts as silent

    enum Tab : int { kTabSpeed, kTabFrame, kTabSound, kTabMusic };
    int tab = kTabSpeed;

    void Seek(double s);
    void Toggle(); // play or pause from where it is
    void Preview(); // the kept part only, from its start
    void Commit(); // the handles as they are become a step undo can go back to
    void Undo(), Redo(), Revert();
    bool Edited() const;
    void SaveOver(), SaveCopy(), ShrinkCopy(), Bin(), StartName(), EndName(bool keep);
    void Say(const std::wstring& s) { said = s, saidAt = GetTickCount64(); }

    Model& model;
    std::wstring path, name, game;
    HWND hwnd = nullptr;
    float scale = 1, W = 0, H = 0;
    bool done = false, leaving = false, dirty = true;
    double leaveAt = 0, openedAt = 0, lastFrame = 0;
    HANDLE leaveTick = nullptr;

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

    ComPtr<IMFDXGIDeviceManager> manager;
    UINT managerToken = 0;
    ComPtr<IMFMediaEngine> engine;
    ComPtr<IUnknown> notify;
    ComPtr<ID3D11Texture2D> videoTex;
    ComPtr<ID2D1Bitmap1> videoBmp;
    DWORD vw = 0, vh = 0;
    bool haveFrame = false, playing = false, previewing = false;
    double len = 0, at = 0; // the clip's length and where it is, seconds

    Cut cut; // what's kept
    std::vector<Cut> steps; // undo's history, steps[stepAt] is now
    size_t stepAt = 0;
    int dragging = 0; // 1 the start handle, 2 the end, 3 the playhead
    bool busy = false; // a save or a rename on its way: the tools wait
    bool naming = false;
    std::wstring nameEdit;
    std::wstring said;
    ULONGLONG saidAt = 0;
    const wchar_t* tipNow = nullptr; // what the control under the mouse does, this frame
    const wchar_t* tipWas = nullptr; // and the last frame: the same one long enough, it shows
    D2D1_RECT_F tipRect{}, tipAt{}; // under the mouse now; where the showing tip points
    std::wstring tipText, toast; // what's showing, kept while it fades out
    double tipSince = 0;
    D2D1_RECT_F barRect{}; // the bar as last drawn: the toast sits over it
    bool binHeld = false, binFired = false; // the bin pressed, and whether that hold went through
    bool leaveAsked = false; // esc with an unsaved trim: once more leaves

    std::shared_ptr<const sys::audiofx::Pcm> gamePcm, songPcm; // the clip's sound, and the song the edit has now
    bool soundAsked = false, noSound = false, songBusy = false, muted = false;
    std::vector<double> beats; // that song's, its own seconds
    std::map<std::wstring, std::shared_ptr<const sys::audiofx::Pcm>> songs; // every song read this session: undo can bring one back
    std::map<std::wstring, std::vector<double>> songBeats;
    std::wstring songFor; // the song songPcm and beats are
    float panelScroll = 0, panelContent = 0; // the panel scrolls when the screen is short
    double takeStarted = 0;
    ComPtr<IMFMediaEngine> mixEngine; // plays the mix, muted picture's sound under it
    ComPtr<IUnknown> mixNotify;
    std::vector<std::wstring> mixFiles; // made this session: deleted on leaving
    int mixWant = 0, mixMade = 0; // versions: asked for, and loaded
    bool mixBusy = false, mixing = false; // being made; one is playing
    double mixDue = 0;
    float rateNow = 1; // the speed the clip plays at, for the part it's in
    ComPtr<ID2D1BitmapBrush> videoBrush; // the picture, cropped for a tall or square frame
    D2D1_RECT_F videoRect{}, frameRect{}; // where the picture and its frame were drawn
    bool frameDrag = false;
    D2D1_POINT_2F frameGrab{};
    float grabX = 0.5f, grabY = 0.5f;
    std::unique_ptr<sys::voice::Take> take;
    double takeAt = 0; // out seconds the take started at
    bool knobBusy = false; // a slider held: one undo step when it's let go

    std::shared_ptr<Work> work;
    std::vector<ComPtr<ID2D1Bitmap>> strip; // the timeline's pictures, made on this device
    int stripCount = 0;
    int poll = 0; // frames still to look for a picture: a paused clip's arrives a moment after the seek
    std::wstring info; // the game, the day, the length and the size, under the name
    Access access; // what a screen reader sees
};
