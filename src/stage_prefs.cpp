#include "stageprefs.h"

#include "sys.h"

namespace {
constexpr UINT kKeyMask = 0x7FF; // vk plus the three modifier bits
}

StagePrefs& StagePrefs::Get() {
    static StagePrefs p = [] {
        StagePrefs s;
        s.split = sys::ReadNum(L"StageSplit", 0) ? 1 : 0;
        s.swap = sys::ReadNum(L"StageSwap", 0) ? 1 : 0;
        UINT a = (UINT)sys::ReadNum(L"StageSplitKey", 'S') & kKeyMask, b = (UINT)sys::ReadNum(L"StageSwapKey", 'W') & kKeyMask;
        if (a & 0xFF) s.keySplit = a; // a value without a key in it is stale or hand-edited: the default
        if (b & 0xFF) s.keySwap = b;
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
