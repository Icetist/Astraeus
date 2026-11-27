// The studio's sound work. Everything runs on whole tracks in memory (a minute of 48 khz stereo is 23 MB of floats) on
// a worker thread. Noise is taken out by spectral gating, with the noise learned from the quietest tenth of the clip;
// loudness is bs.1770's k-weighted, gated measure, the one the video sites even out to; speed keeps pitch by wsola, which
// lays overlapping slices of the sound at the new pace where they line up best.
#include "sys_audiofx.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace sys::audiofx {
namespace {

using Cx = std::complex<float>;
constexpr float kPi = 3.14159265f;
constexpr int kFft = 1024, kHop = 256; // noise: 21 ms slices, three quarters overlapping
constexpr float kQuietShare = 0.1f; // the quietest tenth of the slices is taken as the noise
constexpr float kOverLow = 1.0f, kOverSpan = 1.2f; // noise taken out this far above its level: 1x gentle, 2.2x strong
constexpr float kFloorHigh = 0.3f, kFloorSpan = 0.32f; // and never quieter than this: -10 db gentle, -40 db strong
constexpr float kRiseKeep = 0.65f; // a falling gain eases down: the gating doesn't warble
constexpr float kCeiling = 0.95f; // peaks stay under -0.45 dbfs
constexpr int kLookFrames = kRate / 200; // 5 ms ahead
constexpr float kLimitRelease = 0.1f; // seconds to give the gain back
constexpr float kMaxGainDb = 15, kMinGainDb = -20;
constexpr int kStretchN = 1024, kStretchHop = kStretchN / 2, kStretchSeek = 256, kStretchStep = 4; // wsola: slice, its hop out, how far it looks, every how many samples
constexpr double kQuietWin = 0.05; // dead air: looked at in 50 ms windows
constexpr double kQuietPad = 0.2; // a little air left at both ends of a stretch taken out
constexpr double kQuietMinLong = 2.0, kQuietMinSpan = 1.6; // how long a stretch must be: 2 s gentle, 0.4 s strong
constexpr float kQuietAboveLow = 4, kQuietAboveSpan = 8; // db over the floor that still counts as quiet
constexpr float kQuietUnderHigh = 40, kQuietUnderSpan = 20; // db under the loud parts
constexpr float kQuietCapLow = -36, kQuietCapSpan = 16; // never louder than this
constexpr int kBeatN = 1024, kBeatHop = 512;
constexpr float kMinBpm = 70, kMaxBpm = 180, kLikeBpm = 120;
constexpr double kFadeSec = 0.004, kSongIn = 0.15, kSongOut = 2.0; // joins, the song coming in, and going out at the end

void StartMf() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

void Fft(Cx* a, int n, bool inverse) { // in place, radix 2
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2 * kPi / len * (inverse ? 1 : -1);
        Cx wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            Cx w(1, 0);
            for (int k = 0; k < len / 2; ++k) {
                Cx u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; ++i) a[i] /= (float)n;
}

std::vector<float> Hann(int n) { // periodic: overlapped by half it sums to one
    std::vector<float> w((size_t)n);
    for (int i = 0; i < n; ++i) w[(size_t)i] = 0.5f - 0.5f * std::cos(2 * kPi * i / n);
    return w;
}

struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double Run(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

}

bool Decode(const std::wstring& path, Pcm& out, std::wstring& why) {
    StartMf();
    out.clear();
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) return why = L"The file couldn't be opened", false;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) return why = L"It has no sound", false;
    ComPtr<IMFMediaType> want, got;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kCh);
    if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want.Get()))) { // no resampling on offer: float as it comes, resampled below
        want.Reset();
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want.Get()))) return why = L"Its sound can't be read", false;
    }
    if (FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) return why = L"Its sound can't be read", false;
    UINT32 rate = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate), ch = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_NUM_CHANNELS, kCh);
    if (!rate || !ch) return why = L"Its sound can't be read", false;
    std::vector<float> raw;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        ComPtr<IMFMediaBuffer> b;
        if (!s || FAILED(s->ConvertToContiguousBuffer(&b))) continue;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(b->Lock(&data, nullptr, &len))) continue;
        size_t n = len / sizeof(float);
        size_t at = raw.size();
        raw.resize(at + n);
        std::memcpy(raw.data() + at, data, n * sizeof(float));
        b->Unlock();
    }
    size_t frames = raw.size() / ch;
    if (!frames) return why = L"It has no sound", false;
    double step = (double)rate / kRate; // source frames per output frame
    size_t outFrames = (size_t)(frames / step);
    out.resize(outFrames * kCh);
    for (size_t i = 0; i < outFrames; ++i) { // to stereo at 48 khz; mono is heard in both ears
        double pos = i * step;
        size_t a = (size_t)pos, b = (std::min)(a + 1, frames - 1);
        float f = (float)(pos - a);
        for (int c = 0; c < kCh; ++c) {
            UINT32 sc = (std::min)((UINT32)c, ch - 1);
            out[i * kCh + c] = raw[a * ch + sc] * (1 - f) + raw[b * ch + sc] * f;
        }
    }
    return true;
}

void Denoise(Pcm& p, float amount) {
    size_t frames = p.size() / kCh;
    if (frames < (size_t)kFft * 4) return;
    amount = (std::clamp)(amount, 0.0f, 1.0f);
    const float kOver = kOverLow + kOverSpan * amount, kFloorGain = kFloorHigh - kFloorSpan * amount;
    std::vector<float> win = Hann(kFft);
    size_t slices = (frames - kFft) / kHop + 1;
    std::vector<std::pair<float, size_t>> energy(slices); // how loud each slice is: the quietest show what the noise is
    for (size_t s = 0; s < slices; ++s) {
        double e = 0;
        for (int i = 0; i < kFft; ++i) {
            const float* f = &p[(s * kHop + i) * kCh];
            float m = (f[0] + f[1]) * 0.5f * win[(size_t)i];
            e += m * m;
        }
        energy[s] = {(float)e, s};
    }
    size_t quiet = (std::max)((size_t)1, (size_t)(slices * kQuietShare));
    std::nth_element(energy.begin(), energy.begin() + quiet, energy.end());
    const int bins = kFft / 2 + 1;
    std::vector<float> noise((size_t)bins, 0);
    std::vector<Cx> buf((size_t)kFft);
    for (size_t q = 0; q < quiet; ++q) {
        size_t s = energy[q].second;
        for (int i = 0; i < kFft; ++i) {
            const float* f = &p[(s * kHop + i) * kCh];
            buf[(size_t)i] = Cx((f[0] + f[1]) * 0.5f * win[(size_t)i], 0);
        }
        Fft(buf.data(), kFft, false);
        for (int k = 0; k < bins; ++k) noise[(size_t)k] += std::abs(buf[(size_t)k]) / quiet;
    }
    Pcm out(p.size(), 0);
    std::vector<Cx> spec[kCh];
    for (auto& c : spec) c.resize((size_t)kFft);
    std::vector<float> gain((size_t)bins, 1), raw((size_t)bins), last((size_t)bins, 1);
    constexpr float kOla = 1.5f; // hann analysed and hann laid back at a quarter's hop sums to this
    for (size_t s = 0; s < slices; ++s) {
        for (int c = 0; c < kCh; ++c) {
            for (int i = 0; i < kFft; ++i) spec[c][(size_t)i] = Cx(p[(s * kHop + i) * kCh + c] * win[(size_t)i], 0);
            Fft(spec[c].data(), kFft, false);
        }
        for (int k = 0; k < bins; ++k) {
            float mag = (std::abs(spec[0][(size_t)k]) + std::abs(spec[1][(size_t)k])) * 0.5f, n = noise[(size_t)k] * kOver;
            float g = mag > 1e-9f ? (mag * mag - n * n) / (mag * mag) : 0;
            raw[(size_t)k] = (std::max)(kFloorGain, g);
        }
        for (int k = 0; k < bins; ++k) { // a little smoothing across neighbours and over time: no ringing tones left behind
            float g = (raw[(size_t)(std::max)(k - 1, 0)] + raw[(size_t)k] + raw[(size_t)(std::min)(k + 1, bins - 1)]) / 3;
            gain[(size_t)k] = g < last[(size_t)k] ? last[(size_t)k] * kRiseKeep + g * (1 - kRiseKeep) : g;
            last[(size_t)k] = gain[(size_t)k];
        }
        for (int c = 0; c < kCh; ++c) {
            for (int k = 0; k < bins; ++k) {
                spec[c][(size_t)k] *= gain[(size_t)k];
                if (k > 0 && k < kFft / 2) spec[c][(size_t)(kFft - k)] = std::conj(spec[c][(size_t)k]);
            }
            Fft(spec[c].data(), kFft, true);
            for (int i = 0; i < kFft; ++i) out[(s * kHop + i) * kCh + c] += spec[c][(size_t)i].real() * win[(size_t)i] / kOla;
        }
    }
    size_t done = (slices - 1) * kHop + kFft;
    for (size_t i = done * kCh; i < p.size(); ++i) out[i] = p[i]; // the tail past the last whole slice, as it was
    p.swap(out);
}

float Loudness(const Pcm& p) {
    size_t frames = p.size() / kCh;
    constexpr size_t kBlock = kRate * 4 / 10, kStep = kRate / 10; // 400 ms blocks, 100 ms apart
    if (frames < kBlock) return -70;
    std::vector<double> sq(frames, 0);
    for (int c = 0; c < kCh; ++c) { // k-weighting: a high shelf for the head, a high pass for the low end (bs.1770 at 48 khz)
        Biquad shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
        Biquad pass{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
        for (size_t i = 0; i < frames; ++i) {
            double y = pass.Run(shelf.Run(p[i * kCh + c]));
            sq[i] += y * y;
        }
    }
    std::vector<double> sums(frames + 1, 0);
    for (size_t i = 0; i < frames; ++i) sums[i + 1] = sums[i] + sq[i];
    std::vector<double> blocks;
    for (size_t a = 0; a + kBlock <= frames; a += kStep) blocks.push_back((sums[a + kBlock] - sums[a]) / kBlock);
    auto lufs = [](double power) { return -0.691 + 10 * std::log10((std::max)(power, 1e-12)); };
    double sum = 0;
    size_t n = 0;
    for (double b : blocks)
        if (lufs(b) > -70) sum += b, ++n;
    if (!n) return -70;
    double rel = lufs(sum / n) - 10;
    sum = 0, n = 0;
    for (double b : blocks)
        if (lufs(b) > -70 && lufs(b) > rel) sum += b, ++n;
    return n ? (float)lufs(sum / n) : -70;
}

void Limit(Pcm& p) {
    size_t frames = p.size() / kCh;
    if (!frames) return;
    std::vector<float> need(frames);
    for (size_t i = 0; i < frames; ++i) {
        float peak = (std::max)(std::fabs(p[i * kCh]), std::fabs(p[i * kCh + 1]));
        need[i] = peak > kCeiling ? kCeiling / peak : 1.0f;
    }
    std::deque<size_t> low; // the smallest gain needed over the next few milliseconds
    float env = 1;
    const float attack = 1 - std::exp(-1.0f / (kRate * 0.001f)), release = 1 - std::exp(-1.0f / (kRate * kLimitRelease));
    for (size_t i = 0; i < frames + kLookFrames; ++i) {
        if (i < frames) {
            while (!low.empty() && need[low.back()] >= need[i]) low.pop_back();
            low.push_back(i);
        }
        if (i < (size_t)kLookFrames) continue;
        size_t at = i - kLookFrames;
        while (!low.empty() && low.front() < at) low.pop_front();
        float want = low.empty() ? 1.0f : need[low.front()];
        env += (want - env) * (want < env ? attack : release);
        float g = (std::min)(env, need[at]); // never over the ceiling, even before the envelope gets there
        p[at * kCh] *= g;
        p[at * kCh + 1] *= g;
    }
}

void Even(Pcm& p, float target) {
    float now = Loudness(p);
    if (now <= -69) return; // silence stays silent
    float db = (std::clamp)(target - now, kMinGainDb, kMaxGainDb);
    float g = std::pow(10.0f, db / 20);
    for (float& s : p) s *= g;
    Limit(p);
}

Pcm Stretch(const float* in, size_t frames, float speed) {
    if (speed == 1 || frames < (size_t)kStretchN * 2) { // too short to stretch well: picked up at the new pace instead
        size_t outFrames = (size_t)(frames / speed);
        Pcm out(outFrames * kCh);
        for (size_t i = 0; i < outFrames; ++i) {
            size_t s = (std::min)((size_t)(i * speed), frames - 1);
            out[i * kCh] = in[s * kCh], out[i * kCh + 1] = in[s * kCh + 1];
        }
        return out;
    }
    std::vector<float> win = Hann(kStretchN);
    size_t outFrames = (size_t)(frames / speed);
    Pcm out((outFrames + kStretchN) * kCh, 0);
    auto mono = [&](size_t i) { return i < frames ? (in[i * kCh] + in[i * kCh + 1]) * 0.5f : 0.0f; };
    size_t prev = 0;
    long last = (long)(frames - kStretchN);
    for (size_t k = 0;; ++k) {
        size_t at = k * kStretchHop;
        if (at >= outFrames) break;
        size_t pos = 0;
        if (k > 0) {
            double nominal = k * kStretchHop * (double)speed;
            size_t follow = prev + kStretchHop; // what would carry on smoothly from the last slice
            long lo = (std::clamp)((long)(nominal - kStretchSeek), 0L, last), hi = (std::clamp)((long)(nominal + kStretchSeek), lo, last);
            float best = -1e30f;
            pos = (size_t)lo;
            for (long c = lo; c <= hi; c += 2) { // where the next slice lines up best with that
                float score = 0;
                for (int i = 0; i < kStretchN; i += kStretchStep) score += mono(follow + (size_t)i) * mono((size_t)c + (size_t)i);
                if (score > best) best = score, pos = (size_t)c;
            }
        }
        for (int i = 0; i < kStretchN; ++i) {
            size_t s = pos + (size_t)i;
            if (s >= frames) break;
            out[(at + (size_t)i) * kCh] += in[s * kCh] * win[(size_t)i];
            out[(at + (size_t)i) * kCh + 1] += in[s * kCh + 1] * win[(size_t)i];
        }
        prev = pos;
    }
    out.resize(outFrames * kCh);
    return out;
}

std::vector<std::pair<double, double>> Quiet(const Pcm& p, double from, double to, float amount) {
    std::vector<std::pair<double, double>> found;
    amount = (std::clamp)(amount, 0.0f, 1.0f);
    const double kQuietMin = kQuietMinLong - kQuietMinSpan * amount;
    const float kQuietAbove = kQuietAboveLow + kQuietAboveSpan * amount, kQuietUnder = kQuietUnderHigh - kQuietUnderSpan * amount, kQuietCap = kQuietCapLow + kQuietCapSpan * amount;
    size_t win = (size_t)(kQuietWin * kRate), frames = p.size() / kCh;
    size_t a = (size_t)(from * kRate), b = (std::min)((size_t)(to * kRate), frames);
    if (b <= a + win * 4) return found;
    std::vector<float> db;
    for (size_t s = a; s + win <= b; s += win) {
        double e = 0;
        for (size_t i = s; i < s + win; ++i) {
            float m = (p[i * kCh] + p[i * kCh + 1]) * 0.5f;
            e += m * m;
        }
        db.push_back(10 * std::log10((float)(e / win) + 1e-12f));
    }
    std::vector<float> sorted = db;
    std::sort(sorted.begin(), sorted.end());
    float floor = sorted[sorted.size() / 10], loud = sorted[sorted.size() * 95 / 100];
    float line = (std::min)((std::max)(floor + kQuietAbove, loud - kQuietUnder), kQuietCap);
    if (loud - floor < kQuietAbove) return found; // all one level: nothing stands out as dead air
    size_t run = 0;
    for (size_t i = 0; i <= db.size(); ++i) {
        if (i < db.size() && db[i] < line) {
            ++run;
            continue;
        }
        double len = run * kQuietWin;
        if (len >= kQuietMin) {
            double s = from + (double)(i - run) * kQuietWin + kQuietPad, e = from + (double)i * kQuietWin - kQuietPad;
            if (e - s > clipedit::kMinPiece) found.push_back({s, e});
        }
        run = 0;
    }
    return found;
}

std::vector<double> Beats(const Pcm& p) {
    std::vector<double> beats;
    size_t frames = p.size() / kCh;
    if (frames < (size_t)kRate * 4) return beats;
    std::vector<float> win = Hann(kBeatN), env, lastMag(kBeatN / 2 + 1, 0);
    std::vector<Cx> buf((size_t)kBeatN);
    for (size_t s = 0; s + kBeatN <= frames; s += kBeatHop) { // how much new sound arrives in each slice: onsets stand out
        for (int i = 0; i < kBeatN; ++i) buf[(size_t)i] = Cx((p[(s + i) * kCh] + p[(s + i) * kCh + 1]) * 0.5f * win[(size_t)i], 0);
        Fft(buf.data(), kBeatN, false);
        float flux = 0;
        for (int k = 1; k <= kBeatN / 2; ++k) {
            float m = std::log1p(std::abs(buf[(size_t)k]) * 10);
            flux += (std::max)(0.0f, m - lastMag[(size_t)k]);
            lastMag[(size_t)k] = m;
        }
        env.push_back(flux);
    }
    size_t n = env.size();
    std::vector<float> clean(n, 0);
    constexpr int kMean = 16;
    for (size_t i = 0; i < n; ++i) { // above its own running average only: the beat, not the loudness
        float m = 0;
        int c = 0;
        for (int j = -kMean; j <= kMean; ++j)
            if ((long)i + j >= 0 && (long)i + j < (long)n) m += env[(size_t)((long)i + j)], ++c;
        clean[i] = (std::max)(0.0f, env[i] - m / c);
    }
    double perSec = (double)kRate / kBeatHop;
    int lo = (int)(perSec * 60 / kMaxBpm), hi = (int)(perSec * 60 / kMinBpm);
    double bestScore = -1, period = perSec * 60 / kLikeBpm;
    for (int lag = lo; lag <= hi; ++lag) { // the pace the onsets repeat at, a little in favour of common tempos
        double s = 0;
        for (size_t i = (size_t)lag; i < n; ++i) s += clean[i] * clean[i - (size_t)lag];
        double bpm = perSec * 60 / lag, like = std::exp(-0.5 * std::pow(std::log2(bpm / kLikeBpm) / 0.9, 2));
        if (s * like > bestScore) bestScore = s * like, period = lag;
    }
    double bestP = period, bestO = 0;
    bestScore = -1;
    for (double per = period - 1; per <= period + 1; per += 0.02) { // then exactly, so the grid doesn't drift over a whole song
        for (int o = 0; o < (int)per; ++o) {
            double s = 0;
            for (double at = o; at < n; at += per) s += clean[(size_t)at];
            if (s > bestScore) bestScore = s, bestP = per, bestO = o;
        }
    }
    for (double at = bestO; at < n; at += bestP) beats.push_back(at / perSec);
    return beats;
}

Pcm Mix(const clipedit::Edit& e, const Pcm* game, const Pcm* music) {
    size_t frames = (size_t)std::ceil(clipedit::OutLength(e) * kRate);
    Pcm out(frames * kCh, 0);
    if (game && !game->empty()) {
        size_t have = game->size() / kCh, fade = (size_t)(kFadeSec * kRate);
        double outSec = 0;
        for (const clipedit::Piece& pc : e.pieces) {
            double a = (std::max)(pc.from, e.from), b = (std::min)(pc.to, e.to);
            if (pc.cut || b <= a) continue;
            size_t fa = (std::min)((size_t)(a * kRate), have), fb = (std::min)((size_t)(b * kRate), have);
            size_t at = (size_t)std::llround(outSec * kRate);
            outSec += (b - a) / pc.speed;
            if (fb <= fa) continue;
            Pcm part = pc.speed == 1 ? Pcm(game->begin() + (ptrdiff_t)(fa * kCh), game->begin() + (ptrdiff_t)(fb * kCh)) : Stretch(game->data() + fa * kCh, fb - fa, pc.speed);
            size_t n = part.size() / kCh;
            for (size_t i = 0; i < n && at + i < frames; ++i) {
                float g = 1;
                if (i < fade) g = (float)i / fade; // a few milliseconds in and out: no click where parts meet
                else if (n - i < fade) g = (float)(n - i) / fade;
                out[(at + i) * kCh] += part[i * kCh] * g;
                out[(at + i) * kCh + 1] += part[i * kCh + 1] * g;
            }
        }
        if (e.clean) Denoise(out, e.cleanAmt);
        if (e.game != 1)
            for (float& s : out) s *= e.game;
    }
    for (const clipedit::Voice& v : e.voices) {
        if (!v.pcm) continue;
        size_t at = (size_t)std::llround(v.at * kRate), n = v.pcm->size() / kCh;
        for (size_t i = 0; i < n && at + i < frames; ++i) {
            out[(at + i) * kCh] += (*v.pcm)[i * kCh] * e.voice;
            out[(at + i) * kCh + 1] += (*v.pcm)[i * kCh + 1] * e.voice;
        }
    }
    if (music && !music->empty()) {
        size_t at = (size_t)std::llround((std::max)(e.songAt, 0.0) * kRate), n = music->size() / kCh;
        size_t in = (size_t)(kSongIn * kRate), outFade = (size_t)(kSongOut * kRate);
        for (size_t i = 0; i < n && at + i < frames; ++i) {
            size_t pos = at + i;
            float g = e.music;
            if (i < in) g *= (float)i / in;
            if (frames - pos < outFade) g *= (float)(frames - pos) / outFade; // it fades as the clip ends
            out[pos * kCh] += (*music)[i * kCh] * g;
            out[pos * kCh + 1] += (*music)[i * kCh + 1] * g;
        }
    }
    if (e.even) Even(out, kTargetLufs);
    else Limit(out);
    return out;
}

bool WriteWav(const std::wstring& path, const Pcm& p) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<int16_t> pcm(p.size());
    for (size_t i = 0; i < p.size(); ++i) pcm[i] = (int16_t)std::lround((std::clamp)(p[i], -1.0f, 1.0f) * 32767);
    uint32_t data = (uint32_t)(pcm.size() * sizeof(int16_t)), riff = 36 + data, fmtLen = 16, byteRate = kRate * kCh * 2, rate = kRate;
    uint16_t tag = 1, ch = kCh, align = kCh * 2, bits = 16;
    std::vector<uint8_t> head;
    auto put = [&](const void* v, size_t n) { head.insert(head.end(), (const uint8_t*)v, (const uint8_t*)v + n); };
    put("RIFF", 4), put(&riff, 4), put("WAVEfmt ", 8), put(&fmtLen, 4), put(&tag, 2), put(&ch, 2), put(&rate, 4), put(&byteRate, 4), put(&align, 2), put(&bits, 2), put("data", 4), put(&data, 4);
    DWORD wrote = 0;
    bool ok = WriteFile(f, head.data(), (DWORD)head.size(), &wrote, nullptr) && WriteFile(f, pcm.data(), data, &wrote, nullptr) && wrote == data;
    CloseHandle(f);
    return ok;
}

}
