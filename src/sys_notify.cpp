#include "notify.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

#include "notifs.h"

namespace sys {
namespace {

// new notifications come straight from windows' own notification database, the way halobar reads them:
// it's written even with windows' banners switched off, where neither a popup nor the notification listener sees anything
constexpr DWORD kSettleMs = 150; // windows writes the database in a few bursts: read once they've settled
constexpr DWORD kPollMs = 3000; // safety net for a folder change that went missing
constexpr size_t kReadMax = 20; // newest rows looked at per read, a burst of messages fits easily
constexpr size_t kQueueMax = 4; // a flood keeps only the latest few

struct State {
    HWND hwnd = nullptr;
    UINT msg = 0;
    HANDLE change = INVALID_HANDLE_VALUE; // the notifications folder, signalled on every write
    PTP_WAIT wait = nullptr;
    PTP_TIMER settle = nullptr, poll = nullptr;
    std::atomic<bool> live{true}, busy{false};
    std::atomic<int64_t> stamp{0}; // the database's write stamp at the last read
    int64_t lastId = -1; // newest notification already seen, -1 until the first read; only reads touch it
    std::mutex mu; // guards pending
    std::deque<Toast> pending; // several at once show one after another
};

void Arm(PTP_TIMER t, DWORD dueMs, DWORD periodMs) {
    ULARGE_INTEGER due;
    due.QuadPart = (ULONGLONG)(-(LONGLONG)dueMs * 10000); // relative, 100 ns units
    FILETIME ft = {due.LowPart, due.HighPart};
    SetThreadpoolTimer(t, &ft, periodMs, periodMs ? 1000 : 0); // the poll gets a second of slack, windows batches its wakeups
}

void Read(State* s) { // on a pool thread: anything newer than the last one seen goes in line
    if (!s->live || s->busy.exchange(true)) return; // one read at a time, the next change catches up
    int64_t stamp = NotifStamp();
    bool first = s->lastId < 0; // the first read only learns where history ends
    if (stamp != s->stamp.exchange(stamp) || first) {
        std::vector<Notif> fresh = ReadNotifs(kReadMax); // newest first; names come from the shell, never on the ui thread
        int64_t seen = first ? INT64_MAX : s->lastId; // first: nothing counts as new
        if (first) {
            s->lastId = 0;
            for (const Notif& n : fresh) s->lastId = (std::max)(s->lastId, n.id); // by id, not arrival order: a higher id is newer
        }
        size_t added = 0;
        for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) { // oldest first, the order they arrived
            if (it->id <= seen) continue;
            s->lastId = (std::max)(s->lastId, it->id);
            std::lock_guard lock(s->mu);
            s->pending.push_back(Toast{it->app, it->title, it->body});
            while (s->pending.size() > kQueueMax) s->pending.pop_front();
            ++added;
        }
        if (added && s->live && IsWindow(s->hwnd)) PostMessageW(s->hwnd, s->msg, 0, 0);
    }
    s->busy = false;
}

void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_TIMER) { Read(static_cast<State*>(ctx)); }

void CALLBACK OnChange(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WAIT wait, TP_WAIT_RESULT) {
    auto* s = static_cast<State*>(ctx);
    if (!s->live) return;
    FindNextChangeNotification(s->change); // re-arm the folder watch before reading, so no write slips between
    SetThreadpoolWait(wait, s->change, nullptr);
    Arm(s->settle, kSettleMs, 0); // pushed back by each write in a burst
}

}

struct NotifyWatch::Impl {
    std::shared_ptr<State> s;
};

NotifyWatch::NotifyWatch() = default;
NotifyWatch::~NotifyWatch() { Stop(); }

void NotifyWatch::Start(HWND notify, UINT msg) {
    if (impl) return;
    auto s = std::make_shared<State>();
    s->hwnd = notify;
    s->msg = msg;
    wchar_t dir[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Notifications", dir, MAX_PATH);
    s->change = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    s->settle = CreateThreadpoolTimer(OnTimer, s.get(), nullptr);
    s->poll = CreateThreadpoolTimer(OnTimer, s.get(), nullptr);
    if (s->change != INVALID_HANDLE_VALUE && s->settle) {
        s->wait = CreateThreadpoolWait(OnChange, s.get(), nullptr);
        if (s->wait) SetThreadpoolWait(s->wait, s->change, nullptr);
    }
    if (!s->wait && !s->poll) { // nothing would ever look
        impl = std::make_shared<Impl>(Impl{s});
        Stop();
        return;
    }
    if (s->poll) Arm(s->poll, kPollMs, kPollMs);
    if (s->settle) Arm(s->settle, 1, 0); // the first read, off the ui thread: only notifications from now on
    impl = std::make_shared<Impl>(Impl{s});
}

void NotifyWatch::Stop() {
    if (!impl) return;
    State* s = impl->s.get();
    s->live = false;
    if (s->wait) {
        SetThreadpoolWait(s->wait, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(s->wait, TRUE);
        CloseThreadpoolWait(s->wait);
    }
    for (PTP_TIMER t : {s->settle, s->poll}) { // after the wait: it can arm the settle timer
        if (!t) continue;
        SetThreadpoolTimer(t, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(t, TRUE);
        CloseThreadpoolTimer(t);
    }
    if (s->change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(s->change);
    impl.reset(); // no callback holds the state any more
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
