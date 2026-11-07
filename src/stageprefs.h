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

    struct Shape { // one version of the layout as it was arranged: scales, and offsets in shares of the screen
        float coverS = 1, coverX = 0, coverY = 0, textS = 1, textX = 0, textY = 0, lyrX = 0, lyrY = 0, lyrW = 1, lyrH = 1;
        float coverRound = 0.06f, coverGlow = 1; // corners as a share of its side, 0.5 a circle
        float clockOn = 1, clockS = 1, clockX = 0, clockY = 0, clockA = 0.45f;
        float appOn = 1, albumOn = 1, extrasOn = 1, timesOn = 1; // the song's smaller pieces
        float dim = 0; // the room, darker
        float lyrAlign = 0, lyrDim = 0.34f, lyrHaze = 1, lyrAnchor = 0.30f; // 0 left, 1 center, 2 right; the lines out of turn; where the sung line rests
    };
    Shape shape[2]; // 0 a song without lyrics, 1 with them
    static constexpr float kCoverLo = 0.5f, kCoverHi = 1.6f, kTextLo = 0.6f, kTextHi = 1.5f, kLyrWLo = 0.5f, kLyrWHi = 1.3f, kLyrHLo = 0.4f, kLyrHHi = 1.2f;
    static constexpr float kRoundHi = 0.5f, kGlowHi = 1.6f, kClockLo = 0.6f, kClockHi = 3, kClockALo = 0.1f, kDimHi = 0.7f, kLyrDimLo = 0.08f, kLyrDimHi = 0.8f, kHazeHi = 1.5f, kAnchorLo = 0.12f, kAnchorHi = 0.6f;
    void SaveShape();
    static bool Ease(Shape& shown, const Shape& to, float rate, float dt); // true while it's still on its way

    static StagePrefs& Get(); // loaded on first use
    void Flip(int& field, const wchar_t* key);
    void SetKey(UINT& field, const wchar_t* key, UINT combo);
    static UINT Combo(UINT vk); // vk with the modifiers held right now
    static std::wstring Name(UINT combo); // "Ctrl + S"
};
