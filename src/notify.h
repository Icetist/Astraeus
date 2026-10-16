#pragma once
#include <windows.h>
#include <memory>
#include <string>

namespace sys {

struct Toast {
    std::wstring app, title, body;
};

class NotifyWatch { // mirrors new windows notifications; holds nothing unless started
public:
    NotifyWatch();
    ~NotifyWatch();
    NotifyWatch(const NotifyWatch&) = delete;
    NotifyWatch& operator=(const NotifyWatch&) = delete;
    void Start(HWND notify, UINT msg); // ui thread: asks for access if needed, then listens for toast popups
    void Stop();
    bool Pop(Toast& out); // oldest waiting notification, posted msg means one is waiting
    bool More() const; // another one is waiting after this

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
