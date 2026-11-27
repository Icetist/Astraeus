#pragma once
#include <windows.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Browser downloads as they happen, read from the downloads folder: chrome and edge write "name.crdownload" and firefox
// "name.part" until a file is whole. A thread sleeps on the folder; only while one grows does it look at sizes, twice a
// second. Browsers don't say how big a file will be, so it's how much has come and how fast.
namespace sys::downloads {

struct Going {
    std::wstring name; // what it'll be called
    ULONGLONG bytes = 0;
    double perSec = 0; // smoothed
};

struct Done {
    std::wstring name, path;
};

class Watch {
public:
    ~Watch() { Stop(); }
    void Start(HWND notify, UINT msg); // msg is posted as they grow (at most twice a second) and when one finishes
    void Stop();
    bool On() const { return run.joinable(); }
    std::vector<Going> Now() const;
    bool TakeDone(Done& out); // a download that just finished, once each

private:
    struct Part {
        std::wstring file, name;
        ULONGLONG bytes = 0, lastBytes = 0;
        ULONGLONG lastAt = 0;
        double perSec = 0;
    };
    void Run();
    void Seen(const std::wstring& file, bool gone);
    void Measure();
    HWND hwnd = nullptr;
    UINT message = 0;
    HANDLE quit = nullptr;
    std::wstring dir;
    std::thread run;
    mutable std::mutex mu;
    std::vector<Part> parts;
    std::vector<Done> done;
};

}
