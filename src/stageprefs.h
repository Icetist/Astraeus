#pragma once
#include <windows.h>
#include <string>

// The fullscreen player's two screen split and the keys that work it, read once and kept in memory.
// The settings page and the fullscreen player change the same copy, so both always agree.
struct StagePrefs {
    static constexpr UINT kCtrl = 0x100, kAlt = 0x200, kShift = 0x400; // a key: its virtual key code with these held
    int split = 0; // two screens: the song on one, its lyrics on the other
    int swap = 0; // which of the two shows what: 0 the song on the screen it opened on
    UINT keySplit = 'S', keySwap = 'W'; // free in fullscreen: esc, f, f11, space, the arrows and [ ] are taken
    int capture = 0; // settings is waiting for a key: 1 the split's, 2 the swap's; never saved

    static StagePrefs& Get(); // loaded on first use
    void Flip(int& field, const wchar_t* key);
    void SetKey(UINT& field, const wchar_t* key, UINT combo);
    static UINT Combo(UINT vk); // vk with the modifiers held right now
    static std::wstring Name(UINT combo); // "Ctrl + S"
};
