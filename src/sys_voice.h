#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// A voiceover take: the default microphone, turned by windows into 48 khz stereo, kept in memory until it stops.
namespace sys::voice {

class Take {
public:
    Take() = default;
    ~Take();
    Take(const Take&) = delete;
    Take& operator=(const Take&) = delete;
    bool Start(); // false: no microphone, or windows wouldn't open it
    std::shared_ptr<const std::vector<float>> Stop(); // what was said, interleaved stereo
    bool Live() const { return running; }
    float Level() const { return level; } // how loud, lately: 0..1, for a meter

private:
    void Loop();
    std::thread worker;
    std::atomic<bool> stop{false}, running{false};
    std::atomic<int> opened{0}; // 0 waiting, 1 open, -1 failed
    std::atomic<float> level{0};
    std::mutex mu;
    std::vector<float> pcm;
};

}
