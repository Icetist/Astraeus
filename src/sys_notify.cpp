#include "notify.h"

#include <tlhelp32.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <vector>
#include <chrono>
#include <mutex>

namespace sys {
namespace {

namespace WUN = winrt::Windows::UI::Notifications;
using Listener = WUN::Management::UserNotificationListener;
using Access = WUN::Management::UserNotificationListenerAccessStatus;

constexpr auto kSettle = std::chrono::milliseconds(150); // the popup shows a moment before the notification is listed
constexpr DWORD kPollMs = 3000; // safety net for what the popup hook can't see: do not disturb, a popup already on screen

struct State {
    HWND hwnd = nullptr;
    UINT msg = 0;
    HWINEVENTHOOK hook = nullptr; // instant: a toast popup appeared
    PTP_TIMER poll = nullptr; // catches the rest a moment later
    std::wstring db; // windows' notification database, its size and write time tell us when to look
    uint64_t sig = 0;
    std::atomic<bool> live{true}, busy{false};
    std::atomic<int64_t> lastSeen{0}; // creation time of the newest notification already shown
    std::mutex mu; // guards pending
    std::deque<Toast> pending; // several at once (a burst of messages) show one after another
};

std::weak_ptr<State> gState; // the win event hook and the timer get no context of ours

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
        std::vector<std::pair<int64_t, WUN::UserNotification>> fresh; // everything newer than the last one shown, oldest first
        int64_t seen = s->lastSeen.load();
        for (const auto& n : list) {
            int64_t t = n.CreationTime().time_since_epoch().count();
            if (t > seen) fresh.emplace_back(t, n);
        }
        std::sort(fresh.begin(), fresh.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        if (!fresh.empty() && s->live) {
            s->lastSeen = fresh.back().first;
            for (const auto& [t, n] : fresh) {
                Toast out;
                try {
                    out.app = n.AppInfo().DisplayInfo().DisplayName();
                } catch (...) {
                }
                if (auto binding = n.Notification().Visual().GetBinding(WUN::KnownNotificationBindings::ToastGeneric())) {
                    auto texts = binding.GetTextElements();
                    if (texts.Size() > 0) out.title = texts.GetAt(0).Text();
                    if (texts.Size() > 1) out.body = texts.GetAt(1).Text();
                }
                std::lock_guard lock(s->mu);
                s->pending.push_back(std::move(out));
                while (s->pending.size() > 4) s->pending.pop_front(); // a flood keeps only the latest few
            }
            if (IsWindow(s->hwnd)) PostMessageW(s->hwnd, s->msg, 0, 0);
        }
    } catch (...) {
        OutputDebugStringW(L"[notify] fetch failed\n"); // no access, or the platform is busy: just skip this one
    }
    s->busy = false;
}

uint64_t Signature(const std::wstring& db) { // size and write time of the database and its log: one cheap stat each, no listing
    uint64_t sig = 0;
    for (const wchar_t* tail : {L"", L"-wal"}) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW((db + tail).c_str(), GetFileExInfoStandard, &a))
            sig = sig * 1000003 ^ (((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) + ((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow));
    }
    return sig;
}

void CALLBACK OnPoll(PTP_CALLBACK_INSTANCE, PVOID, PTP_TIMER) {
    auto s = gState.lock();
    if (!s) return;
    uint64_t sig = Signature(s->db);
    if (sig == s->sig) return; // nothing written since last look: no notification arrived
    s->sig = sig;
    Fetch(s); // newer-than-last-seen check: a write that wasn't a new notification shows nothing
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
    wchar_t db[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Notifications\\wpndatabase.db", db, MAX_PATH);
    s->db = db;
    s->sig = Signature(s->db);
    s->poll = CreateThreadpoolTimer(OnPoll, nullptr, nullptr);
    if (!s->hook && !s->poll) return;
    gState = s;
    if (s->poll) {
        ULARGE_INTEGER due;
        due.QuadPart = (ULONGLONG)(-(LONGLONG)kPollMs * 10000); // relative, 100 ns units
        FILETIME ft = {due.LowPart, due.HighPart};
        SetThreadpoolTimer(s->poll, &ft, kPollMs, 1000); // a second of slack lets windows batch the wakeup with others
    }
    impl = std::make_shared<Impl>(Impl{s});
}

void NotifyWatch::Stop() {
    if (!impl) return;
    impl->s->live = false;
    if (impl->s->hook) UnhookWinEvent(impl->s->hook);
    if (impl->s->poll) {
        SetThreadpoolTimer(impl->s->poll, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(impl->s->poll, TRUE);
        CloseThreadpoolTimer(impl->s->poll);
    }
    gState.reset();
    impl.reset();
}

bool NotifyWatch::Pop(Toast& out) {
    if (!impl) return false;
    std::lock_guard lock(impl->s->mu);
    if (impl->s->pending.empty()) return false;
    out = std::move(impl->s->pending.front());
    impl->s->pending.pop_front();
    return true;
}

bool NotifyWatch::More() const {
    if (!impl) return false;
    std::lock_guard lock(impl->s->mu);
    return !impl->s->pending.empty();
}

}
