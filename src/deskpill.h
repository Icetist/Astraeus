#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "anim.h"
#include "history.h"
#include "model.h"
#include "theme.h"
#include "ui.h"

// The long pill on the desktop: its own small window, on any edge, stuck to it or floating clear of it. While it's being
// arranged the window takes the whole screen and shows the wallpaper, so the pill is set where it will really sit.
// The normal island and the panel know nothing of its edge or its float. With nothing to animate it draws nothing.
class DeskPill {
public:
    explicit DeskPill(Model& model);
    ~DeskPill();
    DeskPill(const DeskPill&) = delete;
    DeskPill& operator=(const DeskPill&) = delete;

    bool Create();
    void Want(bool up, HMONITOR on); // the drawer's verdict: the desktop is what's under it, or not
    bool Up() const { return wantUp; } // it's there or on its way: the island steps aside
    void Edit(HMONITOR on, int from = Model::kEditFromLong); // from: which pill was right clicked, for the tab it opens on
    void Studio(HMONITOR on); // the same stage, the wallpaper filling the screen, for arranging the widgets
    bool Editing() const { return editing || leaving; }
    bool MouseOn() const; // the cursor's on its window
    HWND Window() const { return hwnd; } // for the shelf: a carry over the pill opens it
    RECT Spot(HMONITOR on); // where it sits, in screen pixels
    void Second(); // what it shows may have changed
    void Raise() { if (hwnd) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); } // over what just went fullscreen
    void Refresh() { if (wantUp || Editing()) Poke(); } // the theme moved: it paints only when told
    void OnDisplayChange(HMONITOR on = nullptr);
    void SetOverApp(bool v) { // it wears its own body over an app
        if (v == onApp) return;
        onApp = v;
        if (FillNow() == kFillClear) inkDue = lastFrame + 0.05;
        if (!wantUp) return;
        if (!Editing()) shapeShift = true, FitWindow(); // room for both while it changes over
        Poke();
    }
    void LookBehind() { if (wantUp && !Editing()) SampleInk(); } // windows moved, or the wallpaper changed: clear's ink looks again now

    bool Needs() const { return dirty; }
    HANDLE Wait() const { return frameWait; }
    void Frame(const Theme& shell);

    std::function<void()> onOpen, onEdit, onDone; // a click, a right click, the editor gone
    std::function<void()> onLook, onStudioDone; // the editor wants another theme showing; the widgets editor gone

private:
    struct Spec { // what's kept; a working copy while editing, saved as each drag lands
        struct Shape { // what it can be on its own in each place
            float len = 560, gap = 10;
            bool floats = true;
            float thick = 30; // and with it, how big its type is
        } shape[2]; // on the desktop, not on the desktop
        int edge = 0;
        float pos = 500;
        bool desk = true; // the long pill on the desktop; false: the regular one there
        bool apps = false; // the long pill when not on desktop; false: the regular one there
        int fill = 0, fillApps = 0; // its body on the desktop, and over an app
        std::vector<int> items; // the set showing now
        std::vector<int> itemsOther; // the other context's set, while split
        bool split = false; // its own widgets when not on desktop
        int mid = Model::kMidAuto, midOther = Model::kMidAuto; // what holds the middle, for items and for itemsOther
    };
    int itemsCtx = 0; // which context spec.items holds now: 0 the desktop's, 1 over apps
    void SyncItems(); // the set for the context showing: swapped in as it changes
    const std::vector<int>& DeskSet() const { return itemsCtx == 0 ? spec.items : spec.itemsOther; }
    const std::vector<int>& AppsSet() const { return itemsCtx == 0 ? spec.itemsOther : spec.items; }
    int DeskMid() const { return itemsCtx == 0 ? spec.mid : spec.midOther; }
    int AppsMid() const { return itemsCtx == 0 ? spec.midOther : spec.mid; }
    int Anchor(const std::vector<int>& items) const; // the item holding the middle, or -1: one centred row
    struct Pose { // where it's drawn: its edge side's middle, turned to its edge
        float ax = 0, ay = 0, angle = 0, len = 0, thick = 0, flat = 0;
    };
    struct Placed { // an item laid out along the pill
        int kind;
        float x, w;
        std::wstring text;
        int icon;
        bool idle; // nothing to show right now: only the editor draws it, faint, so it can be moved
    };
    enum class Drag { None, Move, Len, Thick, Item };
    enum Fill { kFillTheme, kFillAccent, kFillGlass, kFillClear };

    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Poke() { dirty = true; }
    void Watch(); // what it shows against what it showed: a frame only when that changed; the next lyric line is waited for to the moment
    int Ctx() const { return spec.apps && (editing || leaving ? previewApps : onApp) ? 1 : 0; } // which of its two it is now; the editor shows the tab that's picked
    Spec::Shape& Sh() { return spec.shape[Ctx()]; }
    const Spec::Shape& Sh() const { return spec.shape[Ctx()]; }
    int FillNow() const { return Ctx() ? spec.fillApps : spec.fill; }

    void Measure(HMONITOR on); // the screen's size in dips
    void DropAppItems();
    void Span(int edge, float& lo, float& hi) const; // the stretch of that edge it may sit on
    Pose Goal(int ctx) const; // where the spec puts that one
    Pose Goal() const { return Goal(Ctx()); }
    D2D1_RECT_F Reach(float pad) const; // the room both of its shapes take together
    bool StepRest(float dt); // resting: eases over to the other shape as the place changes
    void Grow(const std::vector<int>& items, float least = 0); // items are shared: both have to fit them, each at its own thickness
    float NeedAll(const std::vector<int>& items); // the most either shape in use needs for these
    void SetPreview(bool apps); // the editor's tab: which of the two it shows and changes
    void FitSong(std::vector<Placed>& all, size_t from, size_t to, float room, float gap, float thick); // a long title gives way, nothing else does
    D2D1_MATRIX_3X2_F Local(const Pose& p) const;
    D2D1_POINT_2F ToLocal(const Pose& p, D2D1_POINT_2F screen) const;
    D2D1_RECT_F Box(const Pose& p, float pad) const; // its bounds on the screen, in dips
    void FitWindow(); // resting: just big enough; editing: the whole screen

    bool EnsureDev();
    void FreeDev();
    bool SizeBuffers(UINT w, UINT h);

    void LoadWall(bool sharp); // the wallpaper for this screen, for the editor and for glass
    D2D1_RECT_F WallRect() const; // where it lands, as windows places it
    void SampleInk(); // clear: black or white, from what's on screen right beside it

    std::vector<Placed> LayOut(const std::vector<int>& items, float len, float thick);
    float NeedLen(const std::vector<int>& items, float thick); // the shortest it can be with these items
    void Text(int kind, std::wstring& text, int& icon, bool& idle) const;
    float ItemW(int kind, const std::wstring& text, int icon, bool idle, float thick); // along the pill
    bool Has(int kind) const;
    void SyncCover(); // the song's art, only while the cover item is on it
    bool StepBars(float dt, double now); // true while they move
    ComPtr<ID2D1Geometry> BuildBody(float len, float thick, float flat) const;
    void DrawPill(const Theme& t, const Pose& p, float alpha);

    // the editor, in deskpill_edit.cpp
    bool StepEdit(float dt);
    void DrawBackdrop();
    void DrawGuides(const Theme& t);
    void DrawGrips(const Theme& t);
    void DrawBar(const Theme& t);
    void PressPill();
    void DragNow();
    void Land(); // the drag ended: clamp, save
    void Commit(); // the working copy into the model and the registry
    void Finish();
    void Leave(); // faded out: back to a plain pill
    bool Seg(const Theme& t, const char* key, const D2D1_RECT_F& r, const wchar_t* const* names, int n, int& value);
    D2D1_RECT_F BarRect() const;
    std::vector<int> Offered() const;
    void DrawFloater(const Theme& t);
    bool TabLong() const { return previewApps ? spec.apps : spec.desk && model.pillDesk; } // the tab picked wears the long pill: there's something to arrange
    bool askLong = false; // opened from the regular pill with the long one nowhere: it asks before anything shows
    void DrawAsk(const Theme& t);
    void DrawBarHead(const Theme& t, const D2D1_RECT_F& bar); // the panel's top row: place, form, own widgets, undo, reset, done
    bool BarButton(const Theme& t, const char* key, const D2D1_RECT_F& r, const wchar_t* label, bool primary);
    void DrawStudio(const Theme& t);

    static constexpr int kEscKey = 0x52B1; // while editing: the pill never takes the keyboard, a hotkey reaches it anyway

    Model& model;
    Spec spec;
    History hist; // the editor's undo: the whole spec
    int editRun = 0; // bumps as the editor opens: its undo starts afresh
    History::Snap SpecSnap() const;
    void SpecLoad(const History::Snap& s);
    HWND hwnd = nullptr;
    HMONITOR screen = nullptr;
    RECT monitor = {}, work = {};
    float scale = 1, MW = 0, MH = 0;
    D2D1_POINT_2F view{}; // the window's corner on the screen, dips
    bool onApp = false, previewApps = false;
    bool wantUp = false, editing = false, leaving = false, dirty = false, hot = false, tracking = false;
    anim::Spring appear; // 0 away .. 100 in place
    Pose pose; // what's drawn; follows Goal while editing
    Pose rest; // what's drawn at rest
    bool shapeShift = false; // going from one shape to the other: the window has room for both
    bool studio = false; // the stage holds the widgets, not the pill
    float intro = 0; // the editor's backdrop, 0 the real desktop .. 1 the wallpaper
    float inkDark = 0, inkGoal = 0; // clear's ink, eased
    double inkDue = 0; // a look at what's behind, once it has settled
    bool inkAfterIntro = false; // and once the editor's backdrop is up
    double lastFrame = 0;
    std::wstring sig; // what a resting pill last showed

    Input input;
    Ui ui;
    LPCWSTR cursor = IDC_HAND;
    std::vector<Placed> placed; // this frame's items, for hit tests
    int midAt = -1; // which of them holds the middle, where a sung line goes
    Placed midLast{}, midWas{}; // the middle as last drawn; what it was as a fade began, going out
    bool midLastOn = false, midHad = false, midFading = false;
    std::wstring midKey; // a sung line by its words, anything else by its kind: the clock ticking isn't a change
    double midSince = 0;

    Drag drag = Drag::None;
    bool pressedOnBar = false, moved = false, offPill = false, bareDown = false;
    float grabX = 0, grabY = 0, grabLen = 0;
    int dragKind = -1;
    float shake = 0; // an item that won't fit: the pill says no
    bool magnetMid = false; // the drag is held at the edge's middle
    bool midZone = false; // an item held over the pill's middle: let go and it holds it
    bool midSnapped = false; // the middle as it was when this drag began, taken once
    int midAtPick = Model::kMidAuto, holderAtPick = -1; // the saved choice, and the item that held the middle then
    int wallFor = 0; // 0 none, 1 glass only, 2 sharp too

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<IDXGISwapChain2> swap;
    ComPtr<ID2D1Bitmap1> buf, wall;
    ComPtr<ID2D1Bitmap> wallBlur;
    D2D1_SIZE_F wallSize{}; // in pixels
    Color wallColor{0.1f, 0.1f, 0.12f, 1}; // no picture: windows' plain color
    DWORD wallPos = 0;
    ComPtr<IDCompositionDesktopDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    HANDLE frameWait = nullptr;
    UINT bufW = 0, bufH = 0;
    std::unique_ptr<Canvas> canvas;
    ComPtr<ID2D1Geometry> body;
    ComPtr<ID2D1Bitmap1> cover;
    ComPtr<ID2D1Bitmap> coverBlur;
    uint64_t coverId = ~0ull;
    float bars[4] = {}; // the small visualizer's levels, eased
    float bodyLen = -1, bodyThick = -1, bodyFlat = -1;
};
