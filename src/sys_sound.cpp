#include "sys_sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace sound {
namespace {

constexpr int kRate = 32000; // plenty for soft bells, a third less memory than cd rate
constexpr float kPi = 3.14159265f;
constexpr float kGain = 0.32f; // under the music, never startling
constexpr float kAttack = 0.004f; // a bell's strike: quick, but no click
constexpr float kPartial2 = 2.01f, kPartial3 = 3.98f; // a bell's overtones, a hair off whole numbers
constexpr float kTail = 0.08f; // every sound fades its last moment, so it never ends on a click

struct Note {
    float hz, at, loud, fall; // pitch, when it strikes, how loud, how fast it dies away
};

// note names, for reading the cues below
constexpr float kC5 = 523.25f, kE5 = 659.26f, kG5 = 783.99f, kC6 = 1046.5f, kE6 = 1318.5f, kG6 = 1568.0f, kA6 = 1760.0f;

std::vector<uint8_t> Wav(const std::vector<int16_t>& pcm) { // a plain 16 bit mono wave, as PlaySound reads from memory
    auto put32 = [](std::vector<uint8_t>& b, uint32_t v) {
        for (int s = 0; s < 32; s += 8) b.push_back((uint8_t)(v >> s));
    };
    auto put16 = [](std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v), b.push_back((uint8_t)(v >> 8)); };
    uint32_t bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> b;
    b.reserve(44 + bytes);
    b.insert(b.end(), {'R', 'I', 'F', 'F'}), put32(b, 36 + bytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}), put32(b, 16);
    put16(b, 1), put16(b, 1), put32(b, kRate), put32(b, kRate * 2), put16(b, 2), put16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'}), put32(b, bytes);
    size_t at = b.size();
    b.resize(at + bytes);
    std::memcpy(b.data() + at, pcm.data(), bytes);
    return b;
}

std::vector<int16_t> Finish(std::vector<float>& mix) {
    std::vector<int16_t> pcm(mix.size());
    float len = (float)mix.size() / kRate;
    for (size_t i = 0; i < mix.size(); ++i) {
        float t = (float)i / kRate, tail = (len - t) / kTail;
        float v = mix[i] * kGain * (tail < 1 ? (std::max)(0.0f, tail) : 1);
        pcm[i] = (int16_t)std::lround((std::clamp)(v, -1.0f, 1.0f) * 32767);
    }
    return pcm;
}

std::vector<uint8_t> Bells(std::initializer_list<Note> notes, float seconds) {
    std::vector<float> mix((size_t)(kRate * seconds));
    for (const Note& n : notes)
        for (size_t i = (size_t)(n.at * kRate); i < mix.size(); ++i) {
            float t = (float)i / kRate - n.at;
            float env = (1 - std::exp(-t / kAttack)) * std::exp(-t * n.fall);
            mix[i] += n.loud * env *
                      (std::sin(2 * kPi * n.hz * t) + 0.35f * std::sin(2 * kPi * n.hz * kPartial2 * t) * std::exp(-t * n.fall) +
                       0.12f * std::sin(2 * kPi * n.hz * kPartial3 * t) * std::exp(-t * n.fall * 2)); // the overtones fade first, as a real bell's
        }
    return Wav(Finish(mix));
}

std::vector<uint8_t> Needle() { // a low thump as the stylus meets the groove, a tick, then a second of crackle and hiss fading out
    constexpr float kSec = 1.3f, kThumpHz = 55, kThumpFall = 28, kTickSec = 0.004f, kHiss = 0.03f, kHissFall = 1.8f, kPopOdds = 0.9988f, kPopAmp = 0.6f, kPopFall = 1.2f, kPopDecay = 0.86f;
    std::vector<float> mix((size_t)(kRate * kSec));
    uint32_t seed = 0x2545F491u;
    auto noise = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    };
    float pop = 0;
    for (size_t i = 0; i < mix.size(); ++i) {
        float t = (float)i / kRate;
        float v = std::sin(2 * kPi * kThumpHz * t) * std::exp(-t * kThumpFall) * 1.5f;
        if (t < kTickSec) v += noise() * 1.2f * (1 - t / kTickSec);
        v += noise() * kHiss * std::exp(-t * kHissFall); // surface noise
        if ((noise() + 1) / 2 > kPopOdds) pop = kPopAmp * std::exp(-t * kPopFall); // dust: a crackle now and then, fewer as it settles
        v += noise() * pop;
        pop *= kPopDecay;
        mix[i] = v;
    }
    return Wav(Finish(mix));
}

const std::vector<uint8_t>& Made(Cue cue) { // each made once, on first use, and kept: PlaySound reads it while it plays
    static std::vector<uint8_t> made[5];
    std::vector<uint8_t>& w = made[(int)cue];
    if (!w.empty()) return w;
    switch (cue) {
    case Cue::Timer: w = Bells({{kC6, 0, 0.8f, 3.2f}, {kG6, 0.16f, 0.7f, 2.6f}}, 1.6f); break;
    case Cue::FocusDone: w = Bells({{kG5, 0, 0.7f, 2.8f}, {kE5, 0.2f, 0.7f, 2.6f}, {kC5, 0.4f, 0.8f, 2.2f}}, 1.9f); break;
    case Cue::BreakDone: w = Bells({{kC5, 0, 0.6f, 3}, {kE5, 0.12f, 0.6f, 3}, {kG5, 0.24f, 0.65f, 3}, {kC6, 0.36f, 0.8f, 2.4f}}, 1.8f); break;
    case Cue::Alarm: w = Bells({{kE6, 0, 0.8f, 3.5f}, {kA6, 0.15f, 0.8f, 3.5f}, {kE6, 0.6f, 0.8f, 3.5f}, {kA6, 0.75f, 0.85f, 2.8f}}, 1.5f); break;
    case Cue::Needle: w = Needle(); break;
    }
    return w;
}

}

void Play(Cue cue) {
    const std::vector<uint8_t>& w = Made(cue);
    if (!w.empty()) PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}
