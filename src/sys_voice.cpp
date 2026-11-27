// A voiceover take from the default microphone. Windows converts whatever the microphone gives into 48 khz stereo
// float, so the take lays straight into the clip's mix.
#include "sys_voice.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

namespace sys::voice {
namespace {

constexpr UINT32 kRate = 48000, kCh = 2;
constexpr REFERENCE_TIME kBuffer = 200000; // 20 ms
constexpr DWORD kWaitMs = 100, kOpenMs = 3000;
constexpr float kLevelFall = 0.9f; // the meter sinks back between words

}

Take::~Take() {
    stop = true;
    if (worker.joinable()) worker.join();
}

bool Take::Start() {
    if (running) return true;
    stop = false, opened = 0, level = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        pcm.clear();
    }
    if (worker.joinable()) worker.join();
    running = true;
    worker = std::thread([this] { Loop(); });
    for (DWORD waited = 0; opened == 0 && waited < kOpenMs; waited += 10) Sleep(10); // a moment: the microphone opens or it doesn't
    if (opened != 1) {
        stop = true;
        worker.join();
        running = false;
        return false;
    }
    return true;
}

std::shared_ptr<const std::vector<float>> Take::Stop() {
    stop = true;
    if (worker.joinable()) worker.join();
    running = false;
    std::lock_guard<std::mutex> lock(mu);
    return std::make_shared<const std::vector<float>>(std::move(pcm));
}

void Take::Loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX fmt{WAVE_FORMAT_IEEE_FLOAT, (WORD)kCh, kRate, kRate * kCh * sizeof(float), (WORD)(kCh * sizeof(float)), 32, 0};
    bool ok = ready && SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) &&
              SUCCEEDED(devices->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) &&
              SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) &&
              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, kBuffer, 0, &fmt, nullptr)) &&
              SUCCEEDED(client->SetEventHandle(ready)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
    opened = ok ? 1 : -1;
    while (ok && !stop) {
        WaitForSingleObject(ready, kWaitMs);
        UINT32 frames = 0;
        while (!stop && SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            size_t n = (size_t)frames * kCh;
            float peak = 0;
            {
                std::lock_guard<std::mutex> lock(mu);
                size_t at = pcm.size();
                pcm.resize(at + n, 0.0f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    std::memcpy(pcm.data() + at, data, n * sizeof(float));
                    for (size_t i = at; i < at + n; ++i) peak = (std::max)(peak, std::fabs(pcm[i]));
                }
            }
            level = (std::max)(peak, level * kLevelFall);
            capture->ReleaseBuffer(frames);
        }
    }
    if (client) client->Stop();
    capture.Reset(), client.Reset(), device.Reset(), devices.Reset();
    if (ready) CloseHandle(ready);
    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
}

}
