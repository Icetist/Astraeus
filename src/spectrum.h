#pragma once
#include <memory>

namespace sys {

// Live frequency bands of whatever is playing, from a loopback of the default output.
// Exists only while something shows it: the capture thread starts with the object and stops with it.
class Spectrum {
public:
    static constexpr int kBands = 64;

    Spectrum();
    ~Spectrum();
    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;

    void Read(float (&bands)[kBands]) const; // 0..1 per band, low to high frequency

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
