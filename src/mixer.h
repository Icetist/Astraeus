#pragma once
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct MixApp {
    std::wstring key; // stable per app: its exe path, or "system" for system sounds
    std::wstring name; // what people call it ("Spotify", "System sounds")
    float volume = 1; // 0..1, the app's own level under the master volume
    bool muted = false;
    bool playing = false; // making sound right now
    float peak = 0; // 0..1, its loudest session at the last Meter()
    std::wstring route; // the output it was sent to, empty when it follows the default
    std::shared_ptr<const std::vector<uint32_t>> icon; // 32x32 premultiplied bgra, null if none
};

// Per-app volume, from Windows' audio sessions on every active output. Holds nothing until Open, frees everything on Close.
class Mixer {
public:
    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;
    bool Open();
    void Close();
    bool IsOpen() const;
    void Refresh(); // re-reads apps and levels; cheap, call about once a second while shown
    void Meter(); // re-reads only each app's peak: cheaper still, for live meters and ducking
    const std::vector<MixApp>& Apps() const;
    std::wstring Device() const; // the default output, "Speakers (Realtek(R) Audio)"
    void SetVolume(const std::wstring& key, float v); // every session of that app
    void SetMuted(const std::wstring& key, bool muted);
    bool HasApp(const std::wstring& key) const; // a session for that exe (the lowercased path), as of the last refresh
    float VolumeOf(const std::wstring& key) const; // its level 0..1, -1 if none or closed
    bool Route(const std::wstring& key, const std::wstring& deviceId); // send the app to that output, empty: back to the default
    static bool CanRoute(); // windows' per-app output routing is reachable

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
