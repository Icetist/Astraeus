#include "notify.h"

#include <tlhelp32.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace sys {
namespace {

namespace WUN = winrt::Windows::UI::Notifications;
using Listener = WUN::Management::UserNotificationListener;
using Access = WUN::Management::UserNotificationListenerAccessStatus;

constexpr auto kSettle = std::chrono::milliseconds(150); // the popup shows a moment before the notification is listed

struct State {
    HWND hwnd = nullptr;
    UINT msg = 0;
    HWINEVENTHOOK hook = nullptr;
    std::atomic<bool> live{true}, busy{false};
    std::atomic<int64_t> lastSeen{0}; // creation time of the newest notification already shown
    std::mutex mu; // guards pending
    bool has = false;
    Toast pending;
};

std::weak_ptr<State> gState; // the win event hook has no context pointer

DWORD ToastHost() { // windows shows toast popups from this process, listening only to it keeps wakeups rare
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !pid; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"ShellExperienceHost.exe")) pid = pe.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

winrt::fire_and_forget Fetch(std::shared_ptr<State> s) { // read the newest notification once, after a popup appeared
    if (s->busy.exchange(true)) co_return;
    try {
        co_await winrt::resume_after(kSettle);
        auto list = co_await Listener::Current().GetNotificationsAsync(WUN::NotificationKinds::Toast);
        WUN::UserNotification best{nullptr};
        int64_t bestT = s->lastSeen.load();
        for (const auto& n : list) {
            int64_t t = n.CreationTime().time_since_epoch().count();
            if (t > bestT) {
                bestT = t;
                best = n;
            }
        }
        if (best && s->live) {
            s->lastSeen = bestT;
            Toast out;
            try {
                out.app = best.AppInfo().DisplayInfo().DisplayName();
            } catch (...) {
            }
            if (auto binding = best.Notification().Visual().GetBinding(WUN::KnownNotificationBindings::ToastGeneric())) {
                auto texts = binding.GetTextElements();
                if (texts.Size() > 0) out.title = texts.GetAt(0).Text();
                if (texts.Size() > 1) out.body = texts.GetAt(1).Text();
            }
            {
                std::lock_guard lock(s->mu);
                s->pending = std::move(out);
                s->has = true;
            }
            if (IsWindow(s->hwnd)) PostMessageW(s->hwnd, s->msg, 0, 0);
        }
    } catch (...) {
        OutputDebugStringW(L"[notify] fetch failed\n"); // no access, or the platform is busy: just skip this one
    }
    s->busy = false;
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (!h || obj != OBJID_WINDOW || child != CHILDID_SELF) return;
    wchar_t cls[40] = {};
    if (!GetClassNameW(h, cls, 40) || wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return; // toast popups are core windows
    if (auto s = gState.lock()) Fetch(s); // newer-than-last-seen check filters other shell windows opening
}

}

struct NotifyWatch::Impl {
    std::shared_ptr<State> s;
};

NotifyWatch::NotifyWatch() = default;
NotifyWatch::~NotifyWatch() { Stop(); }

void NotifyWatch::Start(HWND notify, UINT msg) {
    if (impl) return;
    try {
        auto l = Listener::Current();
        Access st = l.GetAccessStatus();
        if (st == Access::Denied) return; // the user said no in windows settings
        if (st == Access::Unspecified) l.RequestAccessAsync(); // windows asks once; until allowed, fetches just fail quietly
    } catch (...) {
        OutputDebugStringW(L"[notify] listener unavailable\n");
        return;
    }
    auto s = std::make_shared<State>();
    s->hwnd = notify;
    s->msg = msg;
    s->lastSeen = winrt::clock::now().time_since_epoch().count(); // only notifications from now on
    s->hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, ToastHost(), 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!s->hook) return;
    gState = s;
    impl = std::make_shared<Impl>(Impl{s});
}

void NotifyWatch::Stop() {
    if (!impl) return;
    impl->s->live = false;
    UnhookWinEvent(impl->s->hook);
    gState.reset();
    impl.reset();
}

bool NotifyWatch::Pop(Toast& out) {
    if (!impl) return false;
    std::lock_guard lock(impl->s->mu);
    if (!impl->s->has) return false;
    out = std::move(impl->s->pending);
    impl->s->has = false;
    return true;
}

}
