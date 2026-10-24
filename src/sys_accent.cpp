#include "accent.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <atomic>

namespace sys {

namespace WUV = winrt::Windows::UI::ViewManagement;

struct WinAccent::Impl {
    WUV::UISettings ui{nullptr}; // made on first use, freed with Stop
    WUV::UISettings::ColorValuesChanged_revoker changed;
    std::shared_ptr<std::atomic<HWND>> notify = std::make_shared<std::atomic<HWND>>(nullptr); // the event fires on a pool thread
    UINT msg = 0;
    bool Make() {
        if (ui) return true;
        try {
            ui = WUV::UISettings();
        } catch (...) {
            OutputDebugStringW(L"[accent] UISettings unavailable\n");
        }
        return ui != nullptr;
    }
};

WinAccent::WinAccent() : impl(std::make_unique<Impl>()) {}

WinAccent::~WinAccent() { Stop(); }

int WinAccent::Read() {
    bool held = impl->ui != nullptr;
    if (!impl->Make()) return -1;
    int rgb = -1;
    try {
        auto c = impl->ui.GetColorValue(WUV::UIColorType::Accent);
        rgb = c.R << 16 | c.G << 8 | c.B;
    } catch (...) {
        OutputDebugStringW(L"[accent] GetColorValue failed\n");
    }
    if (!held && !impl->changed) impl->ui = nullptr; // a one-off read keeps nothing around
    return rgb;
}

void WinAccent::Watch(HWND notify, UINT msg) {
    impl->notify->store(notify);
    impl->msg = msg;
    if (impl->changed || !impl->Make()) return;
    try {
        auto target = impl->notify;
        UINT m = msg;
        impl->changed = impl->ui.ColorValuesChanged(winrt::auto_revoke, [target, m](auto&&, auto&&) {
            if (HWND h = target->load()) PostMessageW(h, m, 0, 0); // also fires for light/dark switches: the drawer just re-reads
        });
    } catch (...) {
        OutputDebugStringW(L"[accent] ColorValuesChanged unavailable\n");
    }
}

void WinAccent::Stop() {
    impl->notify->store(nullptr);
    impl->changed.revoke();
    impl->ui = nullptr;
}

}
