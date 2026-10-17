#include "picker.h"

#include <cstdint>
#include <cwchar>

namespace sys::picker {
namespace {

constexpr int kGrid = 11; // pixels around the cursor shown in the loupe
constexpr int kCell = 10; // each one this big
constexpr int kLoupe = kGrid * kCell; // 110
constexpr int kW = kLoupe + 16, kH = kLoupe + 46; // loupe plus a margin and the hex underneath
constexpr UINT_PTR kTick = 1;

HWND win = nullptr, notifyTo = nullptr;
UINT notifyMsg = 0;
HHOOK mouse = nullptr, keys = nullptr;
HFONT font = nullptr;
COLORREF under = 0; // the color under the cursor right now

void Finish(bool picked) {
    if (mouse) UnhookWindowsHookEx(mouse);
    if (keys) UnhookWindowsHookEx(keys);
    mouse = keys = nullptr;
    if (win) DestroyWindow(win);
    win = nullptr;
    if (font) DeleteObject(font);
    font = nullptr;
    if (picked && OpenClipboard(nullptr)) { // "#RRGGBB" on the clipboard
        wchar_t hex[8];
        swprintf_s(hex, L"#%02X%02X%02X", GetRValue(under), GetGValue(under), GetBValue(under));
        EmptyClipboard();
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(hex))) {
            if (void* p = GlobalLock(mem)) {
                memcpy(p, hex, sizeof(hex));
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            } else {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
    }
    if (notifyTo) PostMessageW(notifyTo, notifyMsg, picked ? (WPARAM)(GetRValue(under) << 16 | GetGValue(under) << 8 | GetBValue(under)) : 0, picked ? 1 : 0);
}

void Paint() { // the pixels around the cursor, big, with the middle one framed, and its hex below; drawn once per tick while picking
    POINT pt;
    GetCursorPos(&pt);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kW, -kH, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(dc, dib);
    RECT all = {0, 0, kW, kH};
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetStretchBltMode(dc, COLORONCOLOR); // hard pixels, no smoothing
    StretchBlt(dc, 8, 8, kLoupe, kLoupe, screen, pt.x - kGrid / 2, pt.y - kGrid / 2, kGrid, kGrid, SRCCOPY); // no CAPTUREBLT: our own layered loupe isn't in the picture
    HDC one = CreateCompatibleDC(screen); // the exact pixel, read the same way
    HBITMAP px = CreateCompatibleBitmap(screen, 1, 1);
    HGDIOBJ o1 = SelectObject(one, px);
    BitBlt(one, 0, 0, 1, 1, screen, pt.x, pt.y, SRCCOPY);
    under = GetPixel(one, 0, 0);
    SelectObject(one, o1);
    DeleteObject(px);
    DeleteDC(one);
    RECT mid = {8 + (kGrid / 2) * kCell, 8 + (kGrid / 2) * kCell, 8 + (kGrid / 2 + 1) * kCell, 8 + (kGrid / 2 + 1) * kCell};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FrameRect(dc, &mid, white);
    DeleteObject(white);
    RECT sw = {8, kLoupe + 16, 30, kLoupe + 38}; // a swatch of the color, then its hex
    HBRUSH cb = CreateSolidBrush(under);
    FillRect(dc, &sw, cb);
    DeleteObject(cb);
    wchar_t hex[8];
    swprintf_s(hex, L"#%02X%02X%02X", GetRValue(under), GetGValue(under), GetBValue(under));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 245));
    HGDIOBJ of = SelectObject(dc, font);
    RECT tr = {38, kLoupe + 14, kW - 6, kLoupe + 40};
    DrawTextW(dc, hex, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    auto* p = static_cast<uint32_t*>(bits);
    for (int i = 0; i < kW * kH; ++i) p[i] |= 0xFF000000; // gdi leaves alpha at 0: everything here is opaque
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    POINT at = {pt.x + 22, pt.y + 22}; // beside the cursor, flipped at the screen's edges
    if (at.x + kW > mi.rcWork.right) at.x = pt.x - 22 - kW;
    if (at.y + kH > mi.rcWork.bottom) at.y = pt.y - 22 - kH;
    SIZE size = {kW, kH};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(win, screen, &at, &size, dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && win) {
        if (wp == WM_LBUTTONDOWN) {
            Paint(); // the color exactly where the click landed
            Finish(true);
            return 1; // the click picks, it doesn't reach the window below
        }
        if (wp == WM_RBUTTONDOWN) {
            Finish(false);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && win && wp == WM_KEYDOWN && reinterpret_cast<KBDLLHOOKSTRUCT*>(lp)->vkCode == VK_ESCAPE) {
        Finish(false);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == kTick) {
        Paint();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}

bool Active() { return win != nullptr; }

void Start(HWND notify, UINT msg) {
    if (win) return;
    notifyTo = notify;
    notifyMsg = msg;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"AstraeusPicker";
        registered = RegisterClassExW(&wc) != 0;
    }
    win = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, L"AstraeusPicker", L"", WS_POPUP, 0, 0, kW, kH,
                          nullptr, nullptr, inst, nullptr);
    if (!win) return;
    font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, inst, 0);
    keys = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, inst, 0);
    if (!mouse) { // without the hook a click couldn't be caught: don't leave a loupe stuck on screen
        Finish(false);
        return;
    }
    Paint();
    ShowWindow(win, SW_SHOWNOACTIVATE);
    SetTimer(win, kTick, 16, nullptr); // only while picking
}

}
