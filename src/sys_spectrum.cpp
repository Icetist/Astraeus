#include "spectrum.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sys {

namespace {

constexpr int kN = 2048; // fft size: ~43 ms at 48 kHz, enough resolution for the low bands
constexpr int kHop = 1024; // a new spectrum every 1024 samples, ~47 per second, plenty for eased light
constexpr REFERENCE_TIME kBuf = 200000; // 20 ms capture buffer
constexpr float kLoHz = 32, kHiHz = 16000;
constexpr float kFloorDb = -72, kTopDb = -12; // mapped to 0..1
constexpr float kPi = 3.14159265f;

bool IsFloat(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

void Fft(std::vector<std::complex<float>>& a, const std::vector<std::complex<float>>& tw) { // in place, radix 2
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i) { // bit reversal
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k) {
                std::complex<float> u = a[i + k], v = a[i + k + len / 2] * tw[k * step];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

}

struct Spectrum::Impl {
    std::thread worker;
    std::atomic<bool> stop{false};
    mutable std::mutex lock;
    float bands[kBands] = {};

    std::vector<float> ring = std::vector<float>(kN), window = std::vector<float>(kN);
    std::vector<std::complex<float>> buf = std::vector<std::complex<float>>(kN), tw = std::vector<std::complex<float>>(kN / 2);
    int head = 0, fresh = 0;

    Impl() {
        for (int i = 0; i < kN; ++i) window[i] = 0.5f - 0.5f * std::cos(2 * kPi * i / (kN - 1)); // hann
        for (int k = 0; k < kN / 2; ++k) tw[k] = std::polar(1.0f, -2 * kPi * k / kN);
        worker = std::thread([this] { Run(); });
    }

    ~Impl() {
        stop = true;
        if (worker.joinable()) worker.join();
    }

    void Push(float v, float rate) {
        ring[head] = v;
        head = (head + 1) % kN;
        if (++fresh >= kHop) {
            fresh = 0;
            Analyze(rate);
        }
    }

    void Analyze(float rate) {
        for (int i = 0; i < kN; ++i) buf[i] = {ring[(head + i) % kN] * window[i], 0};
        Fft(buf, tw);
        float out[kBands];
        float binHz = rate / kN, norm = 4.0f / kN; // hann halves amplitude, one-sided doubles it
        for (int b = 0; b < kBands; ++b) { // log spaced bands, peak bin inside each
            float f0 = kLoHz * std::pow(kHiHz / kLoHz, (float)b / kBands), f1 = kLoHz * std::pow(kHiHz / kLoHz, (float)(b + 1) / kBands);
            int i0 = (std::max)(1, (int)(f0 / binHz)), i1 = (std::max)(i0, (std::min)(kN / 2 - 1, (int)(f1 / binHz)));
            float peak = 0;
            for (int i = i0; i <= i1; ++i) peak = (std::max)(peak, std::abs(buf[i]));
            float db = 20 * std::log10(peak * norm + 1e-9f) + 6.0f * b / kBands; // gentle tilt, highs carry less energy
            out[b] = std::clamp((db - kFloorDb) / (kTopDb - kFloorDb), 0.0f, 1.0f);
        }
        std::lock_guard<std::mutex> g(lock);
        std::copy(out, out + kBands, bands);
    }

    void Silence() {
        std::lock_guard<std::mutex> g(lock);
        std::fill(bands, bands + kBands, 0.0f);
    }

    bool Capture() { // one session on the current default output, returns when it ends or we stop
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> cap;
        WAVEFORMATEX* fmt = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf())) || FAILED(client->GetMixFormat(&fmt)))
            return false;
        bool isFloat = IsFloat(fmt);
        int ch = fmt->nChannels, bits = fmt->wBitsPerSample;
        float rate = (float)fmt->nSamplesPerSec;
        bool ok = (isFloat && bits == 32) || (!isFloat && bits == 16);
        ok = ok && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBuf, 0, fmt, nullptr)) &&
             SUCCEEDED(client->GetService(IID_PPV_ARGS(&cap))) && SUCCEEDED(client->Start());
        CoTaskMemFree(fmt);
        if (!ok) return false;
        ULONGLONG quiet = GetTickCount64();
        while (!stop) {
            Sleep(15); // polled: loopback events are unreliable when nothing plays
            UINT32 packet = 0;
            if (FAILED(cap->GetNextPacketSize(&packet))) break; // device changed or vanished, reopen the new default
            if (packet == 0 && GetTickCount64() - quiet > 120) { // nothing playing: no packets arrive at all
                Silence();
                quiet = GetTickCount64();
            }
            while (packet > 0 && !stop) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return true;
                bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                for (UINT32 f = 0; f < frames; ++f) { // mono mixdown
                    float sum = 0;
                    if (!silent)
                        for (int c = 0; c < ch; ++c)
                            sum += isFloat ? reinterpret_cast<const float*>(data)[f * ch + c] : reinterpret_cast<const int16_t*>(data)[f * ch + c] / 32768.0f;
                    Push(sum / ch, rate);
                }
                cap->ReleaseBuffer(frames);
                quiet = GetTickCount64();
                if (FAILED(cap->GetNextPacketSize(&packet))) return true;
            }
        }
        client->Stop();
        return true;
    }

    void Run() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop) {
            if (!Capture()) Silence();
            for (int i = 0; i < 25 && !stop; ++i) Sleep(20); // brief pause before reopening
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }
};

Spectrum::Spectrum() : impl(std::make_unique<Impl>()) {}
Spectrum::~Spectrum() = default;

void Spectrum::Read(float (&bands)[kBands]) const {
    std::lock_guard<std::mutex> g(impl->lock);
    std::copy(impl->bands, impl->bands + kBands, bands);
}

}
