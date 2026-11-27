// Astraeus' cues are additive synthesis: a few struck notes, each a handful of partials dying away at their own rates, with a faint
// detune between the ears so the tail shimmers and one soft echo for room. Every cue has its own run of notes, so each is told
// apart by ear alone: a game may hold the whole screen, and the sound is then all there is. Each style plays them its own way too,
// in its own register and pace, so the three don't sound alike.
#include "cues.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace sys::cues {
namespace {

constexpr int kRate = 44100, kChannels = 2, kBits = 16;
constexpr float kTau = 6.28318530718f;
constexpr float kDetune = 0.0032f; // the right ear a hair sharp: the tail beats slowly
constexpr float kEchoSec = 0.19f, kEchoGain = 0.26f; // one soft repeat, crossed to the other ear
constexpr float kPeak = 0.86f; // headroom at full volume
constexpr float kFadeSec = 0.45f; // the tail eased to silence, never cut
constexpr float kSilence = 0.0008f; // a partial this quiet is done

struct Partial { float ratio, amp, decay; }; // of the note's pitch; decay in seconds to 1/e
struct Voice { const Partial* parts; int count; };
struct Note { float at, hz, amp, pan, stretch; }; // seconds in; pan -1 left .. 1 right; stretch scales every decay
struct Tune { const Note* notes; int count; float sec; };

constexpr Partial kChime[] = {{1, 1, 0.85f}, {2.76f, 0.42f, 0.42f}, {5.40f, 0.20f, 0.22f}, {8.93f, 0.09f, 0.11f}, {0.5f, 0.22f, 1.1f}}; // a small bell: its overtones aren't whole multiples
constexpr Partial kGlass[] = {{1, 1, 0.42f}, {2.32f, 0.55f, 0.2f}, {4.25f, 0.3f, 0.1f}, {6.63f, 0.16f, 0.06f}}; // a struck wine glass: its modes, bright and quick
constexpr Partial kPulse[] = {{1, 1, 0.6f}, {3, 0.34f, 0.45f}, {5, 0.2f, 0.32f}, {7, 0.13f, 0.24f}, {0.5f, 0.3f, 0.7f}}; // a soft square wave with a sub below: a synth, not an instrument
constexpr Voice kVoices[kStyles] = {{kChime, 5}, {kGlass, 4}, {kPulse, 5}};
struct Shape { float pitch, pace, attack, tremHz, tremDepth; }; // the same tune moved: register, speed, how a note starts, a throb
constexpr Shape kShapes[kStyles] = {{1, 1, 0.004f, 0, 0}, {2, 0.7f, 0.0015f, 0, 0}, {0.5f, 1.35f, 0.035f, 7.5f, 0.6f}}; // chime as written; glass an octave up and quicker; pulse an octave down, slower, swelling in and throbbing
constexpr float kNyquistUse = 0.42f; // partials above this share of the sample rate would fold back as whistles: left out
constexpr const wchar_t* kStyleNames[kStyles] = {L"Chime", L"Glass", L"Pulse"};

constexpr float kD4 = 293.66f, kE4 = 329.63f, kA4 = 440.0f, kCs5 = 554.37f, kE5 = 659.26f, kA5 = 880.0f, kB5 = 987.77f, kE6 = 1318.51f, kGs6 = 1661.22f;
constexpr Note kClipNotes[] = {{0, kE5, 0.9f, -0.35f, 2.2f}, {0.12f, kB5, 0.8f, 0.3f, 2.2f}, {0.24f, kE6, 0.75f, -0.15f, 2.4f}, {0.40f, kGs6, 0.6f, 0.4f, 2.6f}, {0.40f, kE4, 0.35f, 0, 2.8f}, {0.95f, kE6, 0.28f, 0.1f, 2.4f}}; // an open chord rung upward, then once more, far off: long and unmistakable
constexpr Note kOnNotes[] = {{0, kA4, 0.9f, -0.25f, 1}, {0.09f, kE5, 0.85f, 0.2f, 1}, {0.18f, kA5, 0.9f, 0, 1.3f}}; // a rising fifth and octave: something began
constexpr Note kOffNotes[] = {{0, kA5, 0.85f, 0.2f, 1}, {0.11f, kE5, 0.8f, -0.2f, 1}, {0.24f, kCs5, 0.85f, 0, 1.5f}}; // the same, stepping down to rest
constexpr Note kFailNotes[] = {{0, kD4, 0.9f, 0, 0.45f}, {0.16f, kD4, 0.7f, 0, 0.45f}}; // two low, short knocks: no
constexpr Tune kTunes[kCueCount] = {{kClipNotes, 6, 4.2f}, {kOnNotes, 3, 1.6f}, {kOffNotes, 3, 1.8f}, {kFailNotes, 2, 0.9f}}; // seconds each lasts

#pragma pack(push, 1)
struct Wav { // riff header for 16-bit pcm, then the samples
    char riff[4]; uint32_t size; char wave[4]; char fmt[4]; uint32_t fmtSize; uint16_t tag, channels; uint32_t rate, bytesPerSec; uint16_t align, bits; char data[4]; uint32_t dataSize;
};
#pragma pack(pop)

std::vector<BYTE> Render(Cue cue, int style, int volumePct) {
    const Tune& tune = kTunes[cue];
    const Voice& voice = kVoices[style];
    const Shape& shape = kShapes[style];
    const int frames = (int)(tune.sec * (std::max)(1.0f, shape.pace) * kRate);
    std::vector<float> mix((size_t)frames * kChannels, 0.0f);
    for (int n = 0; n < tune.count; ++n) {
        const Note& note = tune.notes[n];
        const int start = (int)(note.at * shape.pace * kRate);
        const float hz = note.hz * shape.pitch;
        const float gain[kChannels] = {note.amp * (std::min)(1.0f, 1 - note.pan), note.amp * (std::min)(1.0f, 1 + note.pan)};
        for (int p = 0; p < voice.count; ++p) {
            const Partial& part = voice.parts[p];
            const float decay = part.decay * note.stretch;
            if (hz * part.ratio > kRate * kNyquistUse) continue;
            for (int ch = 0; ch < kChannels; ++ch) {
                const float step = kTau * hz * part.ratio * (1 + (ch ? kDetune : 0)) / kRate;
                for (int i = start; i < frames; ++i) {
                    const float t = (float)(i - start) / kRate, env = std::exp(-t / decay);
                    if (env < kSilence) break; // died away: the rest of the cue costs nothing
                    const float throb = 1 - shape.tremDepth * 0.5f * (1 - std::cos(kTau * shape.tremHz * t));
                    mix[(size_t)i * kChannels + ch] += gain[ch] * part.amp * env * throb * (1 - std::exp(-t / shape.attack)) * std::sin(step * (i - start));
                }
            }
        }
    }
    const int echo = (int)(kEchoSec * kRate);
    for (int i = echo; i < frames; ++i) // each ear hears the other's a moment later
        for (int ch = 0; ch < kChannels; ++ch) mix[(size_t)i * kChannels + ch] += kEchoGain * mix[(size_t)(i - echo) * kChannels + (1 - ch)];
    float peak = 0.0001f;
    for (float v : mix) peak = (std::max)(peak, std::fabs(v));
    const float vol = (float)(std::clamp)(volumePct, kVolMin, kVolMax) / kVolMax;
    const float scale = kPeak / peak * vol * vol; // squared: the slider's low end is usable
    const int fade = (int)(kFadeSec * kRate);
    std::vector<BYTE> out(sizeof(Wav) + mix.size() * sizeof(int16_t));
    Wav head{};
    const uint32_t dataSize = (uint32_t)(mix.size() * sizeof(int16_t));
    std::memcpy(head.riff, "RIFF", 4), std::memcpy(head.wave, "WAVE", 4), std::memcpy(head.fmt, "fmt ", 4), std::memcpy(head.data, "data", 4);
    head.size = (uint32_t)(sizeof(Wav) - 8 + dataSize), head.fmtSize = 16, head.tag = 1, head.channels = kChannels, head.rate = kRate;
    head.align = kChannels * kBits / 8, head.bytesPerSec = kRate * head.align, head.bits = kBits, head.dataSize = dataSize;
    std::memcpy(out.data(), &head, sizeof(head));
    int16_t* pcm = reinterpret_cast<int16_t*>(out.data() + sizeof(Wav));
    for (int i = 0; i < frames; ++i) {
        const float tail = i > frames - fade ? (float)(frames - i) / fade : 1.0f;
        for (int ch = 0; ch < kChannels; ++ch) pcm[(size_t)i * kChannels + ch] = (int16_t)std::lround((std::clamp)(mix[(size_t)i * kChannels + ch] * scale * tail, -1.0f, 1.0f) * 32767);
    }
    return out;
}

std::mutex gate; // one cue made and started at a time
std::shared_ptr<const std::vector<BYTE>> playing; // the wave windows is reading from: kept until the next one replaces it
std::mutex made; // the waves already worked out
std::map<int, std::shared_ptr<const std::vector<BYTE>>> waves; // by cue, style and volume: each is worked out once
constexpr size_t kWavesKept = 24; // a few volumes' worth; past that the oldest set is dropped

std::shared_ptr<const std::vector<BYTE>> Wave(Cue cue, int style, int volumePct) { // made once, then reused: under a busy game a cue plays at once
    int key = ((int)cue * kStyles + style) * (kVolMax + 1) + volumePct;
    {
        std::lock_guard<std::mutex> lock(made);
        auto it = waves.find(key);
        if (it != waves.end()) return it->second;
    }
    auto wave = std::make_shared<const std::vector<BYTE>>(Render(cue, style, volumePct));
    std::lock_guard<std::mutex> lock(made);
    if (waves.size() >= kWavesKept) waves.clear();
    waves[key] = wave;
    return wave;
}

}

const wchar_t* StyleName(int style) { return kStyleNames[(std::clamp)(style, 0, kStyles - 1)]; }

void Play(Cue cue, int style, int volumePct, int delayMs) {
    if (cue < 0 || cue >= kCueCount || volumePct <= 0) return;
    style = (std::clamp)(style, 0, kStyles - 1);
    std::thread([cue, style, volumePct, delayMs] { // a few milliseconds of sines: not on the ui thread
        if (delayMs > 0) ::Sleep((DWORD)delayMs);
        auto wave = Wave(cue, style, volumePct);
        std::lock_guard<std::mutex> lock(gate);
        PlaySoundW(nullptr, nullptr, 0); // whatever still rings stops before its memory goes
        playing = std::move(wave);
        PlaySoundW(reinterpret_cast<LPCWSTR>(playing->data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }).detach();
}

void Warm(int style, int volumePct) {
    if (volumePct <= 0) return;
    style = (std::clamp)(style, 0, kStyles - 1);
    std::thread([style, volumePct] { // worked out in the quiet before they're wanted
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (int c = 0; c < kCueCount; ++c) Wave((Cue)c, style, volumePct);
    }).detach();
}

}
