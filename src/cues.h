#pragma once

namespace sys::cues { // astraeus' own cues, made here from sines: no files, nothing borrowed

enum Cue { kClipSaved, kGameOn, kGameOff, kFailed, kCueCount }; // a clip kept (long, ringing), a game found, a game closed, a clip that couldn't be kept
constexpr int kStyles = 3;
const wchar_t* StyleName(int style); // chime, glass, pulse
constexpr int kVolMin = 0, kVolMax = 100, kVolDef = 60;

void Warm(int style, int volumePct); // every cue worked out ahead, off the ui thread: a game starting calls it
void Play(Cue cue, int style, int volumePct, int delayMs = 0); // returns at once: made and played off the ui thread; a cue still ringing gives way to the new one

}
