#include "stageprefs.h"

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <vector>

#include "anim.h"
#include "sys.h"

namespace {
constexpr UINT kKeyMask = 0x7FF; // vk plus the three modifier bits
constexpr wchar_t kShapeVal[] = L"StageShape";
constexpr float kSaved = 1000; // kept as whole thousandths
constexpr wchar_t kMark = L'#'; // a saved list that starts with how many numbers each layout has
using Fields = StagePrefs::Shape;
constexpr float Fields::*kFields[] = {&Fields::coverS, &Fields::coverX, &Fields::coverY, &Fields::textS, &Fields::textX, &Fields::textY, &Fields::lyrX, &Fields::lyrY, &Fields::lyrW, &Fields::lyrH, // new ones go last: a shorter saved list still means what it did
                                      &Fields::coverRound, &Fields::coverGlow, &Fields::clockOn, &Fields::clockS, &Fields::clockX, &Fields::clockY, &Fields::clockA, &Fields::appOn, &Fields::albumOn, &Fields::extrasOn, &Fields::timesOn, &Fields::dim, &Fields::lyrAlign, &Fields::lyrDim, &Fields::lyrHaze, &Fields::lyrAnchor,
                                      &Fields::ctlS, &Fields::ctlX, &Fields::ctlY};

void LoadShape(StagePrefs& p) { // what's missing keeps its default, and what was hand-edited off the screen comes back on
    std::wstring text = sys::ReadText(kShapeVal);
    bool marked = !text.empty() && text[0] == kMark;
    std::vector<long> nums;
    for (const wchar_t* at = text.c_str() + (marked ? 1 : 0); *at;) {
        wchar_t* end = nullptr;
        long n = wcstol(at, &end, 10);
        if (end == at) break;
        nums.push_back(n);
        at = *end ? end + 1 : end;
    }
    size_t per = 0; // numbers a layout has in what was saved: the marker says, or, before it, two layouts shared them
    if (marked && !nums.empty()) per = (size_t)(std::max)(0L, nums.front()), nums.erase(nums.begin());
    else per = nums.size() / 2;
    const size_t have = std::size(kFields);
    for (size_t v = 0; v < (size_t)StagePrefs::kShapes; ++v)
        for (size_t f = 0; f < (std::min)(per, have); ++f)
            if (v * per + f < nums.size()) p.shape[v].*kFields[f] = nums[v * per + f] / kSaved;
    if (!marked) { // vinyl was once arranged in the layout without lyrics: what only vinyl uses says so, and it all moves to vinyl's own
        const StagePrefs::Shape def{};
        StagePrefs::Shape& first = p.shape[0];
        if (first.ctlS != def.ctlS || first.ctlX != def.ctlX || first.ctlY != def.ctlY || first.lyrX != def.lyrX || first.lyrY != def.lyrY || first.lyrW != def.lyrW)
            p.shape[StagePrefs::kVinyl] = first, first = def;
    }
    size_t saved = per ? nums.size() / per : 0;
    if (saved <= (size_t)StagePrefs::kVinylLyr) p.shape[StagePrefs::kVinylLyr] = p.shape[StagePrefs::kVinyl]; // vinyl with lyrics is new: it starts as vinyl was arranged
    for (StagePrefs::Shape& s : p.shape) {
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
        s.ctlS = (std::clamp)(s.ctlS, StagePrefs::kCtlLo, StagePrefs::kCtlHi);
        for (float* off : {&s.coverX, &s.coverY, &s.textX, &s.textY, &s.lyrX, &s.lyrY, &s.clockX, &s.clockY, &s.ctlX, &s.ctlY}) *off = (std::clamp)(*off, -1.0f, 1.0f);
    }
}
}

StagePrefs::Shape StagePrefs::Mix(const Shape& a, const Shape& b, float t) {
    Shape out = a;
    for (auto field : kFields) out.*field = a.*field + (b.*field - a.*field) * t;
    return out;
}

bool StagePrefs::Ease(Shape& shown, const Shape& to, float rate, float dt) {
    constexpr float kNear = 0.0003f; // a fraction of a pixel on any screen
    bool moving = false;
    for (auto field : kFields) moving |= anim::Approach(shown.*field, to.*field, rate, dt, kNear);
    return moving;
}

void StagePrefs::SaveShape() {
    std::wstring text = kMark + std::to_wstring(std::size(kFields)) + L","; // how many numbers each layout has: a new field never shifts one layout into the next
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
        if (UINT w = (UINT)sys::ReadNum(L"StageWordsKey", 'L') & kKeyMask; w & 0xFF) s.keyWords = w;
        LoadShape(s);
        return s;
    }();
    return p;
}

void StagePrefs::Flip(int& field, const wchar_t* key) {
    field = field ? 0 : 1;
    sys::SaveNum(key, field);
}

void StagePrefs::Pick(int which, UINT combo) {
    static const wchar_t* const kNames[kKeys] = {L"StageSplitKey", L"StageSwapKey", L"StageWordsKey"};
    if (which >= 1 && which <= kKeys) SetKey(Key(which), kNames[which - 1], combo);
}

bool StagePrefs::Free(UINT combo, int which) const {
    for (int i = 1; i <= kKeys; ++i)
        if (i != which && combo == KeyOf(i)) return false; // another one's: one key, two jobs
    if (combo & (kCtrl | kAlt | kShift)) return true; // fullscreen's own keys are all plain ones
    static constexpr UINT kTaken[] = {VK_ESCAPE, 'F', VK_F11, VK_SPACE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_OEM_4, VK_OEM_6, 'V', 'C'}; // leave, play, skip, the lyrics' nudge and versions, the vinyl buttons
    return std::find(std::begin(kTaken), std::end(kTaken), combo & 0xFF) == std::end(kTaken);
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
