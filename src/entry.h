#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>

// The small text field at the end of settings' advanced section. What's typed is checked off the ui thread and never kept or logged.
struct Entry {
    static constexpr size_t kMax = 64; // longer than anything worth typing there
    bool focus = false; // taking keystrokes
    std::wstring text;
    double wrongAt = -10; // when the last check failed: a small shake

    static Entry& Get();
    void Type(wchar_t ch); // a WM_CHAR while focused
    void Submit(); // enter: checked on a worker, the field clears at once
    void Drop(); // the panel closed or focus left
    bool Working() const; // a check is still running
    bool Settle(double now); // ui thread, each frame: picks up a finished check; true while one runs

private:
    struct Job {
        std::atomic<int> state{0}; // 0 running, 1 no, 2 yes
    };
    std::shared_ptr<Job> job;
};
