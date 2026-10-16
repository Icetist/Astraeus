#include "tray.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "glyphs.h"
#include "version.h"

namespace tray {
namespace {

constexpr float kW = 252, kPad = 6, kHead = 56, kRow = 38, kSep = 9; // layout in dips
constexpr UINT_PTR kAnimTimer = 1; // drives the switch slide
constexpr UINT_PTR kSpinTimer = 2; // drives the update spinner
constexpr int kSepCmd = 0;
const Color kDanger = Color::Rgb(0xFF8A80); // exit hover tint

struct Item {
    UINT cmd; // kSepCmd = divider
    int glyph;
    const wchar_t* label;
};
const Item kItems[] = {{kOpen, glyph::Overview, L"Open dashboard"},
                       {kConfig, glyph::Edit, L"Edit config"},
                       {kStartup, glyph::Rocket, L"Start with Windows"},
                       {kUpdate, glyph::Update, L"Check for updates"},
                       {kSepCmd, 0, nullptr},
                       {kRestart, glyph::Restart, L"Restart"},
                       {kExit, glyph::Power, L"Exit"}};
constexpr int kCount = sizeof(kItems) / sizeof(kItems[0]); // incl. divider

struct Menu {
    HWND hwnd = nullptr, owner = nullptr;
    Theme t;
    bool startup = false;
    float knob = 0; // switch position 0..1, eased toward startup
    int hover = -1; // item index under mouse or keyboard focus
    bool tracking = false; // leave events armed
    float scale = 1; // dpi / 96
    ComPtr<ID2D1HwndRenderTarget> rt; // plain hwnd target, no swapchain needed
    std::unique_ptr<Canvas> canvas;
};
Menu* g = nullptr; // one menu at a time, null while hidden
Upd upd = Upd::Idle; // outlives the menu: reopening it mid-check still shows the check
std::wstring updTag;

bool UpdBusy() { return upd == Upd::Checking || upd == Upd::Downloading || upd == Upd::Installed; }

std::wstring UpdLabel() {
    switch (upd) {
    case Upd::Checking: return L"Checking\u2026";
    case Upd::Downloading: return L"Downloading " + updTag + L"\u2026";
    case Upd::Installed: return L"Updating to " + updTag + L"\u2026";
    case Upd::Current: return L"You're up to date";
    case Upd::Failed: return L"Couldn't reach GitHub";
    default: return L"Check for updates";
    }
}

void UpdRow(Canvas& c, const Theme& t, const D2D1_RECT_F& r, float cy) { // right side of the update row: version, spinner or a tick
    if (UpdBusy()) {
        float a = (float)(GetTickCount64() % 900) / 900.0f * 360.0f;
        c.Ring({r.right - 24, cy}, 7, t.track, 2);
        c.Arc({r.right - 24, cy}, 7, a, 100, t.accentHi, 2);
    } else if (upd == Upd::Current) {
        c.Icon(glyph::Check, {r.right - 24, cy}, 18, t.accentHi, true);
    } else {
        c.Text(L"v" APP_VER_W, {r.right - 80, r.top, r.right - 12, r.bottom}, Weight::Regular, 11, t.textLow, Align::Right);
    }
}

float Top(int i) { // y of item i
    float y = kHead;
    for (int k = 0; k < i; ++k) y += kItems[k].cmd == kSepCmd ? kSep : kRow;
    return y;
}

float Height() { return Top(kCount) + kPad; } // total menu height

int HitAt(float y) {
    for (int i = 0; i < kCount; ++i) {
        float top = Top(i), h = kItems[i].cmd == kSepCmd ? kSep : kRow;
        if (y >= top && y < top + h) return kItems[i].cmd == kSepCmd ? -1 : i; // dividers aren't clickable
    }
    return -1;
}

void Step(int dir) { // keyboard: move focus, skip dividers, wrap around
    int i = g->hover;
    for (int n = 0; n < kCount; ++n) {
        i = (i + dir + kCount) % kCount;
        if (kItems[i].cmd != kSepCmd) break;
    }
    g->hover = i;
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

void Switch(Canvas& c, const Theme& t, D2D1_POINT_2F o, float k) {
    D2D1_RECT_F r = {o.x - 19, o.y - 11, o.x + 19, o.y + 11}; // 38x22 pill
    c.Fill(r, 11, t.track.Mix(t.accent, k));
    c.Stroke(r, 11, t.hairTop.Mix(t.accentHi.Alpha(0.6f), k), 1);
    c.Dot({r.left + 11 + 16 * k, o.y}, 5.5f + 2.5f * k, t.textDim.Mix(t.onAccent, k)); // knob slides + grows when on
}

void Paint() {
    if (!g->rt) { // created lazily, dropped with the window
        RECT rc;
        GetClientRect(g->hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * g->scale,
            96 * g->scale); // draw in dips
        if (FAILED(gfx::D2D()->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(g->hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g->rt)))
            return;
        g->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g->canvas = std::make_unique<Canvas>(g->rt.Get());
    }
    const Theme& t = g->t;
    Canvas& c = *g->canvas;
    float h = Height();
    g->rt->BeginDraw();
    g->rt->Clear(t.card.D2D()); // opaque card, same as panel
    c.FillV({0, 0, kW, kHead + 20}, 0, t.accent.Alpha(0.12f), t.accent.Alpha(0)); // accent wash behind header
    D2D1_POINT_2F badge = {kPad + 22, kHead / 2}; // sparkle echoes the logo
    c.Dot(badge, 16, t.accent.Alpha(0.20f));
    c.Ring(badge, 16, t.accentHi.Alpha(0.35f), 1);
    c.Icon(glyph::Sparkle, badge, 18, t.accentHi, true);
    c.Text(L"Astraeus", Rect(kPad + 48, 13, 160, 18), Weight::Medium, 14, t.text);
    c.Text(L"Bezel dashboard", Rect(kPad + 48, 30, 160, 14), Weight::Light, 11, t.textLow);

    for (int i = 0; i < kCount; ++i) {
        const Item& it = kItems[i];
        float y = Top(i);
        if (it.cmd == kSepCmd) {
            c.Line({kPad + 12, y + kSep / 2}, {kW - kPad - 12, y + kSep / 2}, t.hairTop, 1, false);
            continue;
        }
        D2D1_RECT_F r = {kPad, y + 1, kW - kPad, y + kRow - 1}; // 1px gap between pills
        float cy = (r.top + r.bottom) / 2;
        bool hot = i == g->hover, danger = it.cmd == kExit;
        if (hot) c.Fill(r, 10, danger ? kDanger.Alpha(0.14f) : t.cardHi.Mix(t.track, 0.6f)); // hover pill
        Color ink = hot && danger ? kDanger : t.text;
        Color icon = hot ? (danger ? kDanger : t.accentHi) : t.textDim;
        c.Icon(it.glyph, {r.left + 22, cy}, 19, icon, hot);
        bool upRow = it.cmd == kUpdate;
        if (upRow && upd == Upd::Failed) ink = hot ? kDanger : t.textDim;
        c.Text(upRow ? UpdLabel() : it.label, {r.left + 44, r.top, r.right - 56, r.bottom}, Weight::Regular, 13, ink);
        if (it.cmd == kStartup) Switch(c, t, {r.right - 30, cy}, g->knob);
        if (upRow) UpdRow(c, t, r, cy);
    }
    c.StrokeV({0, 0, kW, h}, 8, t.hairTop, t.hairBot, 1); // inner rim, dwm rounds the outside
    if (g->rt->EndDraw() == D2DERR_RECREATE_TARGET) { // gpu reset, rebuild next paint
        g->canvas.reset();
        g->rt.Reset();
    }
}

void Pick(int i) {
    if (i < 0) return;
    UINT cmd = kItems[i].cmd;
    if (cmd == kUpdate) { // checks in place: the row tells how it went
        if (UpdBusy()) return;
        SetUpdate(Upd::Checking);
        PostMessageW(g->owner, WM_COMMAND, cmd, 0);
        return;
    }
    PostMessageW(g->owner, WM_COMMAND, cmd, 0); // same ids the old menu used
    if (cmd == kStartup) { // toggle in place, animate the switch
        g->startup = !g->startup;
        SetTimer(g->hwnd, kAnimTimer, 15, nullptr);
        return;
    }
    DestroyWindow(g->hwnd); // any other pick closes
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g) return DefWindowProcW(hwnd, msg, wp, lp); // messages during create
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // d2d paints everything, avoids flicker
    case WM_MOUSEMOVE: {
        if (!g->tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            g->tracking = TrackMouseEvent(&tme) != FALSE;
        }
        int i = HitAt(GET_Y_LPARAM(lp) / g->scale);
        if (i != g->hover) {
            g->hover = i;
            InvalidateRect(hwnd, nullptr, FALSE); // repaint only on change
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g->tracking = false;
        g->hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        Pick(HitAt(GET_Y_LPARAM(lp) / g->scale)); // act on release, like native menus
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd); // esc closes
        else if (wp == VK_DOWN) Step(1);
        else if (wp == VK_UP) Step(-1);
        else if (wp == VK_RETURN || wp == VK_SPACE) Pick(g->hover);
        return 0;
    case WM_TIMER: {
        if (wp == kSpinTimer) {
            if (!UpdBusy()) KillTimer(hwnd, kSpinTimer);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        float target = g->startup ? 1.0f : 0.0f;
        g->knob += (target - g->knob) * 0.3f; // ease toward target each tick
        if (std::fabs(target - g->knob) < 0.01f) {
            g->knob = target;
            KillTimer(hwnd, kAnimTimer); // settled, stop ticking
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) PostMessageW(hwnd, WM_CLOSE, 0, 0); // click outside closes
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        delete g; // releases render target too
        g = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

COLORREF Ref(Color c) { return RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)); }

}

void Show(HWND owner, const Theme& t, bool startup) {
    if (g) DestroyWindow(g->hwnd);
    static ATOM cls = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW; // native popup shadow
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AstraeusTray";
        return RegisterClassExW(&wc);
    }();
    if (!cls) return; // class registration failed
    POINT p; // anchor at the click
    GetCursorPos(&p);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);

    if (upd == Upd::Current || upd == Upd::Failed) upd = Upd::Idle; // last answer is old news
    g = new Menu();
    g->owner = owner;
    g->t = t;
    g->startup = startup;
    g->knob = startup ? 1.0f : 0.0f;
    g->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"AstraeusTray", L"Astraeus", WS_POPUP, p.x, p.y, 1, 1, owner,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g->hwnd) {
        delete g;
        g = nullptr;
        return;
    }
    g->scale = GetDpiForWindow(g->hwnd) / 96.0f; // dpi of the monitor under the cursor
    int w = (int)(kW * g->scale + 0.5f), h = (int)(Height() * g->scale + 0.5f);
    const RECT& wa = mi.rcWork;
    int x = (std::min)((int)p.x, (int)wa.right - w); // keep fully on screen
    int y = p.y - h >= wa.top ? (int)p.y - h : (int)p.y; // above the cursor, below if no room (top taskbar)
    x = (std::max)(x, (int)wa.left); // left edge clamp

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND; // win11 rounded corners, ignored on win10
    DwmSetWindowAttribute(g->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = Ref(t.track);
    DwmSetWindowAttribute(g->hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border)); // subtle themed edge
    SetWindowPos(g->hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g->hwnd); // needed so clicking elsewhere deactivates and closes it
    if (UpdBusy()) SetTimer(g->hwnd, kSpinTimer, 16, nullptr);
}

void SetUpdate(Upd state, const std::wstring& tag) {
    upd = state;
    if (!tag.empty()) updTag = tag;
    if (!g) return;
    if (UpdBusy()) SetTimer(g->hwnd, kSpinTimer, 16, nullptr); // spinner turns while waiting
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

namespace {

constexpr float kAskW = 384, kAskH = 176; // prompt size in dips

struct Prompt {
    HWND hwnd = nullptr, owner = nullptr;
    Theme t;
    int hover = -1; // 0 = not now, 1 = turn on
    bool tracking = false;
    float scale = 1;
    ComPtr<ID2D1HwndRenderTarget> rt;
    std::unique_ptr<Canvas> canvas;
};
Prompt* q = nullptr; // live prompt, null otherwise

D2D1_RECT_F AskBtn(int i) { // 0 left ghost, 1 right accent
    float w = 128, h = 38, gap = 10, right = kAskW - 20, y = kAskH - 20 - h;
    return i == 1 ? D2D1_RECT_F{right - w, y, right, y + h} : D2D1_RECT_F{right - 2 * w - gap, y, right - w - gap, y + h};
}

int AskHit(float x, float y) {
    for (int i = 0; i < 2; ++i)
        if (Inside(AskBtn(i), {x, y})) return i;
    return -1;
}

void AskPaint() {
    if (!q->rt) { // same lazy target setup as the menu
        RECT rc;
        GetClientRect(q->hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * q->scale,
            96 * q->scale);
        if (FAILED(gfx::D2D()->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(q->hwnd, D2D1::SizeU(rc.right, rc.bottom)), &q->rt)))
            return;
        q->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        q->canvas = std::make_unique<Canvas>(q->rt.Get());
    }
    const Theme& t = q->t;
    Canvas& c = *q->canvas;
    q->rt->BeginDraw();
    q->rt->Clear(t.card.D2D());
    c.Glow({60, 50}, 220, 150, t.accent.Alpha(0.14f)); // accent bloom top-left
    D2D1_POINT_2F badge = {44, 50};
    c.Dot(badge, 22, t.accent.Alpha(0.20f));
    c.Ring(badge, 22, t.accentHi.Alpha(0.35f), 1);
    c.Icon(glyph::Rocket, badge, 24, t.accentHi, true);
    c.Text(L"Start with Windows?", Rect(80, 28, kAskW - 100, 24), Weight::Medium, 17, t.text);
    c.Text(L"Astraeus can open when you sign in,", Rect(80, 54, kAskW - 100, 18), Weight::Light, 12.5f, t.textDim);
    c.Text(L"so the bezel is always ready.", Rect(80, 72, kAskW - 100, 18), Weight::Light, 12.5f, t.textDim); // manual wrap
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F r = AskBtn(i);
        bool hot = q->hover == i;
        float rad = Height(r) / 2;
        if (i == 1) { // primary: accent gradient + halo on hover
            if (hot) c.Fill(Inset(r, -4, -4), rad + 4, t.accent.Alpha(0.18f));
            c.FillH(r, rad, t.accent, hot ? t.accentHi : t.accentHi.Mix(t.accent, 0.4f));
            c.Text(L"Turn on", r, Weight::Medium, 13, t.onAccent, Align::Center);
        } else { // ghost
            c.Fill(r, rad, hot ? t.cardHi.Mix(t.track, 0.7f) : t.cardHi);
            c.Stroke(r, rad, t.hairTop, 1);
            c.Text(L"Not now", r, Weight::Medium, 13, hot ? t.text : t.textDim, Align::Center);
        }
    }
    c.StrokeV({0, 0, kAskW, kAskH}, 8, t.hairTop, t.hairBot, 1);
    if (q->rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        q->canvas.reset();
        q->rt.Reset();
    }
}

void Answer(bool yes) {
    if (yes) PostMessageW(q->owner, WM_COMMAND, kStartup, 0); // owner flips startup on
    DestroyWindow(q->hwnd);
}

LRESULT CALLBACK AskProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!q) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        AskPaint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!q->tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            q->tracking = TrackMouseEvent(&tme) != FALSE;
        }
        int i = AskHit(GET_X_LPARAM(lp) / q->scale, GET_Y_LPARAM(lp) / q->scale);
        if (i != q->hover) {
            q->hover = i;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        q->tracking = false;
        q->hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, q->hover >= 0 ? IDC_HAND : IDC_ARROW)); // hand over buttons
        return TRUE;
    case WM_LBUTTONUP: {
        int i = AskHit(GET_X_LPARAM(lp) / q->scale, GET_Y_LPARAM(lp) / q->scale);
        if (i >= 0) Answer(i == 1);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) Answer(false);
        else if (wp == VK_RETURN) Answer(true); // enter = turn on
        return 0;
    case WM_CLOSE:
        Answer(false);
        return 0;
    case WM_NCDESTROY:
        delete q;
        q = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}

void Ask(HWND owner, const Theme& t) {
    if (q) return; // already asking
    static ATOM cls = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = AskProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AstraeusAsk";
        return RegisterClassExW(&wc);
    }();
    if (!cls) return;
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), &mi); // screen the user is on
    q = new Prompt();
    q->owner = owner;
    q->t = t;
    q->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"AstraeusAsk", L"Astraeus", WS_POPUP, mi.rcWork.left,
                              mi.rcWork.top, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!q->hwnd) {
        delete q;
        q = nullptr;
        return;
    }
    q->scale = GetDpiForWindow(q->hwnd) / 96.0f;
    int w = (int)(kAskW * q->scale + 0.5f), h = (int)(kAskH * q->scale + 0.5f);
    const RECT& wa = mi.rcWork;
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(q->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = Ref(t.track);
    DwmSetWindowAttribute(q->hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    SetWindowPos(q->hwnd, HWND_TOPMOST, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                 SWP_SHOWWINDOW); // centered on the work area
    SetForegroundWindow(q->hwnd);
}

}
