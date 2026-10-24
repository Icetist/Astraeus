#pragma once
#include <windows.h>
#include <memory>

// the pieces behind astraeus' own volume and brightness pill; each exists only while that option is on
namespace sys {

class VolumeWatch { // the speakers' level and mute, told by windows on every change; msg: wparam level 0..1000, lparam muted
public:
    VolumeWatch();
    ~VolumeWatch();
    VolumeWatch(const VolumeWatch&) = delete;
    VolumeWatch& operator=(const VolumeWatch&) = delete;
    bool Start(HWND notify, UINT msg); // binds to the default output as it is now
    void Stop();
    bool On() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class BrightWatch { // wmi brightness events on laptop panels; msg: wparam percent. desktops have none, it stays quiet
public:
    BrightWatch();
    ~BrightWatch();
    BrightWatch(const BrightWatch&) = delete;
    BrightWatch& operator=(const BrightWatch&) = delete;
    void Start(HWND notify, UINT msg);
    void Stop();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

class VolumeKeys { // a low level hook that takes the volume keys before windows shows its own popup; msg: wparam the key
public:
    ~VolumeKeys() { Stop(); }
    bool Start(HWND notify, UINT msg);
    void Stop();
    bool On() const { return hook != nullptr; }
    static void Usable(bool on); // false: the keys pass through to windows, there's nothing here to change
private:
    HHOOK hook = nullptr;
};

}
