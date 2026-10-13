#include "sys.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace sys {

struct AudioDev::Impl {
    EDataFlow flow = eRender; // speakers or mic
    ComPtr<IMMDeviceEnumerator> devEnum; // created once, reused across refreshes
    ComPtr<IAudioEndpointVolume> vol; // null until Refresh succeeds
};

AudioDev::AudioDev(int flow) { // int keeps mmdeviceapi out of the header
    try {
        impl = std::make_unique<Impl>();
        impl->flow = flow == eCapture ? eCapture : eRender; // anything else -> render
    } catch (...) {
        OutputDebugStringW(L"[sys] AudioEndpoint init failed\n"); // null impl = all calls no-op
    }
}

AudioDev::AudioDev(AudioDev&&) noexcept = default;
AudioDev& AudioDev::operator=(AudioDev&&) noexcept = default;
AudioDev::~AudioDev() = default;

AudioDev AudioDev::Spk() {
    AudioDev ep(eRender);
    ep.Refresh(); // failure ok, Ready() reports it
    return ep;
}

AudioDev AudioDev::Micro() {
    AudioDev ep(eCapture); // default recording device
    ep.Refresh();
    return ep;
}

bool AudioDev::Refresh() { // rebind to current default device
    if (!impl) return false;
    impl->vol.Reset(); // drop old endpoint, default may have changed
    if (!impl->devEnum) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&impl->devEnum));
        if (FAILED(hr)) {
            OutputDebugStringW(L"[sys] MMDeviceEnumerator unavailable (COM initialised?)\n");
            return false;
        }
    }
    ComPtr<IMMDevice> device;
    HRESULT hr = impl->devEnum->GetDefaultAudioEndpoint(impl->flow, eConsole, &device); // eConsole = what the volume flyout shows
    if (FAILED(hr) || !device) return false; // no device plugged in
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                          reinterpret_cast<void**>(impl->vol.GetAddressOf()));
    if (FAILED(hr)) {
        impl->vol.Reset(); // Activate may leave junk on failure
        OutputDebugStringW(L"[sys] IAudioEndpointVolume activation failed\n");
        return false;
    }
    return true;
}

bool AudioDev::Ready() const {
    return impl && impl->vol; // false after device unplug until Refresh
}

float AudioDev::Level() const {
    if (!Ready()) return 0.0f;
    float v = 0.0f;
    if (FAILED(impl->vol->GetMasterVolumeLevelScalar(&v))) return 0.0f; // scalar = perceptual 0..1, not dB
    return std::clamp(v, 0.0f, 1.0f);
}

void AudioDev::SetLevel(float level) {
    if (!Ready()) return;
    const float v = std::clamp(level, 0.0f, 1.0f);
    if (FAILED(impl->vol->SetMasterVolumeLevelScalar(v, nullptr))) { // null context: we don't filter our own notifications
        OutputDebugStringW(L"[sys] SetMasterVolumeLevelScalar failed\n");
        return;
    }
    if (v > 0.0f && Muted()) SetMuted(false); // raising volume unmutes, like the windows flyout
}

bool AudioDev::Muted() const {
    if (!Ready()) return false;
    BOOL m = FALSE;
    if (FAILED(impl->vol->GetMute(&m))) return false;
    return m != FALSE; // win32 BOOL -> bool
}

void AudioDev::SetMuted(bool muted) {
    if (!Ready()) return;
    if (FAILED(impl->vol->SetMute(muted ? TRUE : FALSE, nullptr))) {
        OutputDebugStringW(L"[sys] SetMute failed\n");
    }
}

}
