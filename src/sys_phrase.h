#pragma once
#include <windows.h>

#include <memory>
#include <string>

// Listens for one spoken phrase, such as "astra clip that", and posts msg to notify each time it's heard. Windows' own
// speech engine does the hearing, on this pc with no internet, from the default microphone in shared mode: every other app
// using the microphone carries on as before. It only knows the one phrase, which keeps it light and hard to fool.
namespace sys::phrase {

class Listener {
public:
    Listener();
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    void Start(const std::wstring& phrase, HWND notify, UINT msg); // off the ui thread; a running one with another phrase starts again
    void Stop();
    bool On() const;
    std::wstring Phrase() const;
    std::wstring Why() const; // why it isn't listening, empty when it is or is starting
    struct Impl; // shared with the thread that listens

private:
    std::shared_ptr<Impl> impl;
};

}
