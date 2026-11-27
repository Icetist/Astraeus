#pragma once
#include <string>
#include <utility>
#include <vector>

#include "clipedit.h"

// The studio's sound work: a clip's or a song's sound read into memory, background noise taken down, loudness measured
// the way the video sites measure it and evened out, a part sped up or slowed without changing its pitch, quiet stretches
// and a song's beats found, and the finished clip's sound mixed. All of it slow: worker threads only.
namespace sys::audiofx {

constexpr int kRate = 48000, kCh = 2;
constexpr float kTargetLufs = -14; // what youtube, tiktok and instagram play at
using Pcm = std::vector<float>; // interleaved stereo at kRate

bool Decode(const std::wstring& path, Pcm& out, std::wstring& why); // the first sound track of any file windows can play
void Denoise(Pcm& p, float amount = 0.5f); // a steady hiss, hum or fan learned from the quietest moments, and taken out; amount 0..1, gentle to strong
float Loudness(const Pcm& p); // lufs, gated as bs.1770 has it; -70 for silence
void Even(Pcm& p, float target); // to that loudness, its peaks held under clipping
void Limit(Pcm& p); // peaks held under clipping, nothing else changed
Pcm Stretch(const float* in, size_t frames, float speed); // faster or slower at the same pitch
std::vector<std::pair<double, double>> Quiet(const Pcm& p, double from, double to, float amount = 0.5f); // dead air between from and to, seconds; amount 0..1: higher takes fainter sound and shorter pauses as silence
std::vector<double> Beats(const Pcm& p); // a song's beats, seconds
Pcm Mix(const clipedit::Edit& e, const Pcm* game, const Pcm* music); // the finished clip's sound, on its own timeline
bool WriteWav(const std::wstring& path, const Pcm& p);

}
