#pragma once
#include <windows.h>
#include <string>

// The fullscreen player's two screen split and the keys that work it, read once and kept in memory.
// The settings page and the fullscreen player change the same copy, so both always agree.
struct StagePrefs {
    static constexpr UINT kCtrl = 0x100, kAlt = 0x200, kShift = 0x400; // a key: its virtual key code with these held
    int split = 0; // two screens: the song on one, its lyrics on the other
    int swap = 0; // which of the two shows what: 0 the song on the screen it opened on
    UINT keySplit = 'S', keySwap = 'W', keyWords = 'L'; // free in fullscreen: esc, f, f11, space, the arrows and [ ] are taken; words: the lyrics alone
    static constexpr int kKeys = 3;
    int capture = 0; // settings is waiting for a key: 1 the split's, 2 the swap's, 3 the words'; never saved
    UINT& Key(int which) { return which == 1 ? keySplit : which == 2 ? keySwap : keyWords; } // 1 .. kKeys
    UINT KeyOf(int which) const { return which == 1 ? keySplit : which == 2 ? keySwap : keyWords; }
    void Pick(int which, UINT combo); // that key set and kept
    bool clash = false; // the last key pressed for it was taken: fullscreen's own, or the other one's
    bool Free(UINT combo, int which) const; // a key for that one: not the other's, nor one fullscreen already answers

    struct Shape { // one version of the layout as it was arranged: scales, and offsets in shares of the screen
        float coverS = 1, coverX = 0, coverY = 0, textS = 1, textX = 0, textY = 0, lyrX = 0, lyrY = 0, lyrW = 1, lyrH = 1;
        float coverRound = 0.06f, coverGlow = 1; // corners as a share of its side, 0.5 a circle
        float clockOn = 1, clockS = 1, clockX = 0, clockY = 0, clockA = 0.45f;
        float appOn = 1, albumOn = 1, extrasOn = 1, timesOn = 1; // the song's smaller pieces
        float dim = 0; // the room, darker
        float lyrAlign = 0, lyrDim = 0.34f, lyrHaze = 1, lyrAnchor = 0.30f; // 0 left, 1 center, 2 right; the lines out of turn; where the sung line rests
        float ctlS = 1, ctlX = 0, ctlY = 0; // the vinyl room's buttons, a part of their own there
        float waveX = 0, waveY = 0, waveW = 1, waveH = 1; // the waveform look's band: where it sits, and how wide and tall, each way apart
    };
    static constexpr float kWaveWLo = 0.25f, kWaveWHi = 1.03f, kWaveHLo = 0.3f, kWaveHHi = 1.8f;
    static constexpr float kCtlLo = 0.6f, kCtlHi = 1.6f;
    static constexpr int kShapes = 4, kVinyl = 2, kVinylLyr = 3;
    Shape shape[kShapes]; // 0 a song without lyrics, 1 with them, 2 the vinyl room without, 3 with: each arranged apart, so one never moves another
    static Shape Mix(const Shape& a, const Shape& b, float t); // part way from one layout to the other, field by field
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
