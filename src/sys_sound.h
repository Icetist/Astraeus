#pragma once
// Astraeus' own small sounds, made in memory the first time each is wanted: soft bells instead of windows' system beep.
namespace sound {

enum class Cue {
    Timer, // a countdown ran out: two bright notes
    FocusDone, // a focus block ended, a break begins: notes falling, to ease off
    BreakDone, // the break is over: notes rising, back to it
    Alarm, // an alarm: a brighter pair, played again while it rings
    Needle, // the fullscreen record's arm landing: a thump, a tick, a little crackle
};

void Play(Cue cue); // async; a new one replaces whatever of ours is still playing

}
