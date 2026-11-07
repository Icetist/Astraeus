#include "stageprefs.h"

#include <algorithm>
#include <cwchar>

#include "anim.h"
#include "sys.h"

namespace {
constexpr UINT kKeyMask = 0x7FF; // vk plus the three modifier bits
constexpr wchar_t kShapeVal[] = L"StageShape";
constexpr float kSaved = 1000; // kept as whole thousandths
using Fields = StagePrefs::Shape;
constexpr float Fields::*kFields[] = {&Fields::coverS, &Fields::coverX, &Fields::coverY, &Fields::textS, &Fields::textX, &Fields::textY, &Fields::lyrX, &Fields::lyrY, &Fields::lyrW, &Fields::lyrH, // new ones go last: a shorter saved list still means what it did
                                      &Fields::coverRound, &Fields::coverGlow, &Fields::clockOn, &Fields::clockS, &Fields::clockX, &Fields::clockY, &Fields::clockA, &Fields::appOn, &Fields::albumOn, &Fields::extrasOn, &Fields::timesOn, &Fields::dim, &Fields::lyrAlign, &Fields::lyrDim, &Fields::lyrHaze, &Fields::lyrAnchor};

void LoadShape(StagePrefs& p) { // what's missing keeps its default, and what was hand-edited off the screen comes back on
    std::wstring text = sys::ReadText(kShapeVal);
    const wchar_t* at = text.c_str();
    bool more = true;
    for (StagePrefs::Shape& s : p.shape) {
        for (auto field : kFields) {
            wchar_t* end = nullptr;
            long n = more ? wcstol(at, &end, 10) : 0;
            more = more && end != at;
            if (!more) continue;
            s.*field = n / kSaved;
            at = *end ? end + 1 : end;
        }
        s.coverS = (std::clamp)(s.coverS, StagePrefs::kCoverLo, StagePrefs::kCoverHi);
        s.textS = (std::clamp)(s.textS, StagePrefs::kTextLo, StagePrefs::kTextHi);
        s.lyrW = (std::clamp)(s.lyrW, StagePrefs::kLyrWLo, StagePrefs::kLyrWHi);
        s.lyrH = (std::clamp)(s.lyrH, StagePrefs::kLyrHLo, StagePrefs::kLyrHHi);
        s.coverRound = (std::clamp)(s.coverRound, 0.0f, StagePrefs::kRoundHi);
        s.coverGlow = (std::clamp)(s.coverGlow, 0.0f, StagePrefs::kGlowHi);
        s.clockS = (std::clamp)(s.clockS, StagePrefs::kClockLo, StagePrefs::kClockHi);
        s.clockA = (std::clamp)(s.clockA, StagePrefs::kClockALo, 1.0f);
        s.dim = (std::clamp)(s.dim, 0.0f, StagePrefs::kDimHi);
        s.lyrAlign = (std::clamp)(s.lyrAlign, 0.0f, 2.0f);
        s.lyrDim = (std::clamp)(s.lyrDim, StagePrefs::kLyrDimLo, StagePrefs::kLyrDimHi);
        s.lyrHaze = (std::clamp)(s.lyrHaze, 0.0f, StagePrefs::kHazeHi);
        s.lyrAnchor = (std::clamp)(s.lyrAnchor, StagePrefs::kAnchorLo, StagePrefs::kAnchorHi);
        for (float* off : {&s.coverX, &s.coverY, &s.textX, &s.textY, &s.lyrX, &s.lyrY, &s.clockX, &s.clockY}) *off = (std::clamp)(*off, -1.0f, 1.0f);
    }
}
}

bool StagePrefs::Ease(Shape& shown, const Shape& to, float rate, float dt) {
    constexpr float kNear = 0.0003f; // a fraction of a pixel on any screen
    bool moving = false;
    for (auto field : kFields) moving |= anim::Approach(shown.*field, to.*field, rate, dt, kNear);
    return moving;
}

void StagePrefs::SaveShape() {
    std::wstring text;
    for (const Shape& s : shape)
        for (auto field : kFields) text += std::to_wstring((long)(s.*field * kSaved + (s.*field < 0 ? -0.5f : 0.5f))) + L",";
    sys::SaveText(kShapeVal, text);
}

StagePrefs& StagePrefs::Get() {
    static StagePrefs p = [] {
        StagePrefs s;
        s.split = sys::ReadNum(L"StageSplit", 0) ? 1 : 0;
        s.swap = sys::ReadNum(L"StageSwap", 0) ? 1 : 0;
        UINT a = (UINT)sys::ReadNum(L"StageSplitKey", 'S') & kKeyMask, b = (UINT)sys::ReadNum(L"StageSwapKey", 'W') & kKeyMask;
        if (a & 0xFF) s.keySplit = a; // a value without a key in it is stale or hand-edited: the default
        if (b & 0xFF) s.keySwap = b;
        LoadShape(s);
        return s;
    }();
    return p;
}

void StagePrefs::Flip(int& field, const wchar_t* key) {
    field = field ? 0 : 1;
    sys::SaveNum(key, field);
}

void StagePrefs::SetKey(UINT& field, const wchar_t* key, UINT combo) {
    field = combo & kKeyMask;
    sys::SaveNum(key, (int)field);
}

UINT StagePrefs::Combo(UINT vk) {
    UINT c = vk & 0xFF;
    if (GetKeyState(VK_CONTROL) < 0) c |= kCtrl;
    if (GetKeyState(VK_MENU) < 0) c |= kAlt;
    if (GetKeyState(VK_SHIFT) < 0) c |= kShift;
    return c;
}

std::wstring StagePrefs::Name(UINT combo) {
    std::wstring s;
    if (combo & kCtrl) s += L"Ctrl + ";
    if (combo & kAlt) s += L"Alt + ";
    if (combo & kShift) s += L"Shift + ";
    UINT vk = combo & 0xFF;
    LONG scan = (LONG)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_INSERT ||
        vk == VK_DELETE)
        scan |= 1 << 24; // the extended keys, else windows names the number pad's twin
    wchar_t name[32] = {};
    if (GetKeyNameTextW(scan, name, 32) > 0) s += name;
    else s += L"Key " + std::to_wstring(vk);
    return s;
}
