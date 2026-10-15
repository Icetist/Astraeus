#include "media.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <mutex>

namespace sys {
namespace {

namespace WF = winrt::Windows::Foundation;
namespace MC = winrt::Windows::Media::Control;
namespace WM = winrt::Windows::Media;
namespace SS = winrt::Windows::Storage::Streams;
namespace WR = winrt::Windows::Devices::Radios;

using Manager = MC::GlobalSystemMediaTransportControlsSessionManager;
using Session = MC::GlobalSystemMediaTransportControlsSession;
using RepeatMode = WM::MediaPlaybackAutoRepeatMode; // shared with SMTC enums

constexpr int64_t kMsTicks = 10000; // winrt ticks are 100ns
constexpr int64_t kDayTicks = 24LL * 3600 * 1000 * kMsTicks;
constexpr int64_t kDrift = 1000 * kMsTicks; // 1s tolerance before position counts as changed
constexpr uint64_t kCoverRetry = 4000; // ms to keep refetching art after track change
constexpr uint32_t kMaxCover = 16u << 20; // 16 MB cap on thumbnail
constexpr uint32_t kChunk = 64u << 10; // read size when stream size unknown
constexpr auto kDebounce = std::chrono::milliseconds(40); // debounce bursts of winrt events
constexpr auto kSettleFirst = std::chrono::milliseconds(600); // follow-up reads after a song change, catch late metadata
constexpr auto kSettleLast = std::chrono::milliseconds(1400); // second one lands 2 s after the change
constexpr uint64_t kLoopLeadMs = 1000; // loop restarts this far before the end, ahead of players auto-advancing
constexpr double kLoopEndMs = 250; // paused this close to the end counts as finished, not paused by the user
constexpr uint64_t kLoopSlackMs = 250; // re-arm only when the restart time moves more than this

void Log(const wchar_t* where, const wchar_t* what) noexcept {
    wchar_t buf[512];
    swprintf_s(buf, L"[Astraeus/media] %ls: %ls\n", where, what ? what : L""); // truncates safely
    OutputDebugStringW(buf);
}

void LogErr(const wchar_t* where) noexcept { // only valid inside a catch block
    try {
        throw; // rethrow current exception to classify it
    } catch (const winrt::hresult_error& e) {
        wchar_t buf[384];
        try {
            swprintf_s(buf, L"0x%08X %ls", static_cast<unsigned>(e.code().value), e.message().c_str());
        } catch (...) {
            swprintf_s(buf, L"0x%08X", static_cast<unsigned>(e.code().value)); // message() can throw
        }
        Log(where, buf);
    } catch (const std::exception&) {
        Log(where, L"std::exception"); // what() is narrow, skip it
    } catch (...) {
        Log(where, L"unknown exception");
    }
}

std::wstring ToW(const winrt::hstring& h) { return std::wstring(h.c_str(), h.size()); } // size-based, keeps embedded nulls

std::wstring Lower(std::wstring s) { // by value, returns modified copy
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool IsHex16(const std::wstring& s) { // chromium-style random session ids
    return s.size() == 16 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; });
}

std::wstring AppName(const std::wstring& id) { // aumid or exe path -> display name
    if (id.empty()) return {};
    if (IsHex16(id)) return L"Browser";
    std::wstring seg = id;
    if (auto p = seg.find_last_of(L"\\/"); p != std::wstring::npos && p + 1 < seg.size()) seg = seg.substr(p + 1); // strip dir

    struct Known { const wchar_t* key; const wchar_t* name; };
    static const Known kContains[] = { // substring match on full id
        {L"spotify", L"Spotify"}, {L"zunemusic", L"Media Player"}, {L"zunevideo", L"Movies & TV"},
    };
    const std::wstring low = Lower(seg);
    for (const auto& k : kContains)
        if (low.find(k.key) != std::wstring::npos) return k.name;

    if (auto p = seg.rfind(L'!'); p != std::wstring::npos && p + 1 < seg.size()) seg = seg.substr(p + 1); // uwp "family!app" -> app
    if (seg.size() > 4 && Lower(seg.substr(seg.size() - 4)) == L".exe") seg.resize(seg.size() - 4);
    if (auto p = seg.rfind(L'_'); p != std::wstring::npos && p > 0 && seg.size() - p - 1 == 13 && // 13-char package publisher hash
        std::all_of(seg.begin() + static_cast<std::ptrdiff_t>(p) + 1, seg.end(),
                    [](wchar_t c) { return std::iswdigit(c) || (c >= L'a' && c <= L'z'); }))
        seg.resize(p);
    if (auto p = seg.rfind(L'.'); p != std::wstring::npos && p + 1 < seg.size()) seg = seg.substr(p + 1); // "vendor.app" -> app
    if (seg.empty()) return id;
    if (IsHex16(seg)) return L"Browser";

    static const Known kExact[] = { // exact match after stripping
        {L"chrome", L"Chrome"}, {L"msedge", L"Edge"}, {L"zen", L"Zen"}, {L"firefox", L"Firefox"},
        {L"brave", L"Brave"}, {L"opera", L"Opera"}, {L"vlc", L"VLC"}, {L"vivaldi", L"Vivaldi"},
    };
    const std::wstring lowSeg = Lower(seg);
    for (const auto& k : kExact)
        if (lowSeg == k.key) return k.name;
    seg[0] = static_cast<wchar_t>(std::towupper(seg[0])); // fallback: capitalised segment
    return seg;
}

int64_t ElapsedAt(const Track& t, uint64_t tickMs) { // extrapolate position from last stamp
    double pos = static_cast<double>(t.position);
    if (t.playing && t.stamp && tickMs > t.stamp)
        pos += static_cast<double>(tickMs - t.stamp) * static_cast<double>(kMsTicks) * t.rate; // ms -> ticks, scaled by rate
    if (pos < 0) pos = 0;
    if (t.duration > 0 && pos > static_cast<double>(t.duration)) pos = static_cast<double>(t.duration); // clamp to end
    return static_cast<int64_t>(pos);
}

bool SameTrack(const Track& a, const Track& b) { // true = nothing worth repainting
    if (a.app != b.app || a.title != b.title || a.artist != b.artist || a.album != b.album) return false;
    if (a.playing != b.playing || a.canPrev != b.canPrev || a.canNext != b.canNext || a.canSeek != b.canSeek ||
        a.canShuf != b.canShuf || a.canRep != b.canRep || a.shuffle != b.shuffle ||
        a.repeat != b.repeat || a.rate != b.rate || a.duration != b.duration ||
        a.coverId != b.coverId) // id compare, not bytes
        return false;
    const int64_t predicted = ElapsedAt(a, b.stamp); // where a should be at b's time
    return std::llabs(predicted - b.position) < kDrift; // only a real seek counts
}

template <class State>
void Ping(const State& s) noexcept { // wake ui thread, it pulls a snapshot
    if (s.closed.load()) return;
    const HWND hwnd = s.notify.load();
    if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, s.msg.load(), 0, 0); // post, never send: called off ui thread
}

struct MediaSt {
    std::atomic<HWND> notify{nullptr};
    std::atomic<UINT> msg{0};
    std::atomic<bool> closed{false}, started{false}, running{false}, pending{false}; // pending/running drive the worker
    std::atomic<uint64_t> settleGen{0}; // bumps per song/app change, older follow-up reads drop out

    mutable std::mutex mu; // guards everything below
    Manager manager{nullptr};
    Session session{nullptr}; // currently hooked session
    Manager::SessionsChanged_revoker listRev;
    Manager::CurrentSessionChanged_revoker curRev;
    Session::MediaPropertiesChanged_revoker propsRev;
    Session::PlaybackInfoChanged_revoker pbRev;
    Session::TimelinePropertiesChanged_revoker tlRev;
    Track track;
    std::wstring selId;
    bool has = false;
    bool sent = false; // first result always pings

    std::wstring coverKey; // worker-only, no lock needed
    uint64_t keyTick = 0; // when coverKey last changed
    uint64_t seq = 0; // coverId source

    bool loop = false; // our own loop, for apps with no repeat of their own (guarded by mu)
    std::wstring loopSel; // session the loop was turned on for
    uint64_t loopGen = 0; // bumps to cancel a pending restart
    uint64_t loopDue = 0; // tick ms the pending restart fires, 0 = none
};
using MediaP = std::shared_ptr<MediaSt>;

void Poke(const MediaP& s) noexcept;

void Kick(const std::weak_ptr<MediaSt>& w) noexcept { // weak so handlers don't keep state alive
    try {
        if (auto s = w.lock()) Poke(s);
    } catch (...) {
        LogErr(L"kick");
    }
}

winrt::fire_and_forget Settle(std::weak_ptr<MediaSt> w, uint64_t gen) { // apps like spotify publish a new song in steps: read again once it has landed
    try {
        for (int step = 0; step < 2; ++step) {
            co_await winrt::resume_after(step == 0 ? kSettleFirst : kSettleLast);
            auto s = w.lock();
            if (!s || s->closed.load() || s->settleGen.load() != gen) co_return; // a newer change restarted the schedule
            Poke(s);
        }
    } catch (...) {
        LogErr(L"settle");
    }
}

void KickSettle(const std::weak_ptr<MediaSt>& w) noexcept { // song or app changed: read now and twice more shortly after
    try {
        if (auto s = w.lock()) {
            Poke(s);
            Settle(w, ++s->settleGen);
        }
    } catch (...) {
        LogErr(L"kick");
    }
}

void CloseMedia(const MediaP& s) noexcept {
    try {
        Manager::SessionsChanged_revoker r1; // locals outlive the lock scope
        Manager::CurrentSessionChanged_revoker r2;
        Session::MediaPropertiesChanged_revoker r3;
        Session::PlaybackInfoChanged_revoker r4;
        Session::TimelinePropertiesChanged_revoker r5;
        Manager mgr{nullptr};
        Session ses{nullptr};
        {
            std::lock_guard lock(s->mu);
            s->closed.store(true);
            r1 = std::move(s->listRev); // move revokers out so they die outside the lock
            r2 = std::move(s->curRev);
            r3 = std::move(s->propsRev);
            r4 = std::move(s->pbRev);
            r5 = std::move(s->tlRev);
            mgr = std::move(s->manager);
            ses = std::move(s->session);
        } // com release happens here, unlocked
    } catch (...) {
        LogErr(L"close");
    }
}

void Hook(const MediaP& s, const Session& sel) { // swap event handlers to the new session
    {
        std::lock_guard lock(s->mu);
        if (s->closed.load() || s->session == sel) return; // already hooked
    }
    Session::MediaPropertiesChanged_revoker a;
    Session::PlaybackInfoChanged_revoker b;
    Session::TimelinePropertiesChanged_revoker c;
    if (sel) {
        std::weak_ptr<MediaSt> w = s;
        try { // register outside the lock, winrt calls can block
            a = sel.MediaPropertiesChanged(winrt::auto_revoke, [w](auto&&, auto&&) { KickSettle(w); }); // new song: metadata may still be landing
            b = sel.PlaybackInfoChanged(winrt::auto_revoke, [w](auto&&, auto&&) { Kick(w); });
            c = sel.TimelinePropertiesChanged(winrt::auto_revoke, [w](auto&&, auto&&) { Kick(w); });
        } catch (...) {
            LogErr(L"hook"); // partial hooks are fine, refresh still polls
        }
    }
    Session::MediaPropertiesChanged_revoker oa; // old handlers revoked at scope end
    Session::PlaybackInfoChanged_revoker ob;
    Session::TimelinePropertiesChanged_revoker oc;
    Session old{nullptr};
    {
        std::lock_guard lock(s->mu);
        if (s->closed.load()) return; // new revokers drop here and unhook
        oa = std::move(s->propsRev); // swap under lock, release after
        ob = std::move(s->pbRev);
        oc = std::move(s->tlRev);
        s->propsRev = std::move(a);
        s->pbRev = std::move(b);
        s->tlRev = std::move(c);
        old = std::move(s->session); // old session ref dropped outside lock
        s->session = sel;
    }
}

void ReadPb(Track& t, const MC::GlobalSystemMediaTransportControlsSessionPlaybackInfo& pb) {
    if (!pb) return;
    t.playing = pb.PlaybackStatus() == MC::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    if (auto c = pb.Controls()) {
        t.canPrev = c.IsPreviousEnabled();
        t.canNext = c.IsNextEnabled();
        t.canSeek = c.IsPlaybackPositionEnabled();
        t.canShuf = c.IsShuffleEnabled();
        t.canRep = c.IsRepeatEnabled();
    }
    if (auto v = pb.IsShuffleActive()) t.shuffle = v.Value(); // IReference, null if app doesn't report
    if (auto r = pb.AutoRepeatMode()) {
        switch (r.Value()) {
            case RepeatMode::Track: t.repeat = 1; break;
            case RepeatMode::List: t.repeat = 2; break;
            default: t.repeat = 0; break;
        }
    }
    if (auto rate = pb.PlaybackRate()) {
        const double d = rate.Value();
        if (d > 0.0 && d < 16.0) t.rate = d; // ignore paused 0 and garbage rates
    }
}

void ReadTl(Track& t, const MC::GlobalSystemMediaTransportControlsSessionTimelineProperties& tl,
                  uint64_t nowMs) {
    t.stamp = nowMs; // default: position valid now
    if (!tl) return;
    const int64_t start = tl.StartTime().count();
    const int64_t end = tl.EndTime().count();
    t.duration = std::max<int64_t>(0, end - start); // normalise to start = 0
    t.position = std::max<int64_t>(0, tl.Position().count() - start);
    if (t.duration > 0) t.position = std::min(t.position, t.duration);
    const WF::DateTime last = tl.LastUpdatedTime(); // when the app reported position
    const int64_t age = (winrt::clock::now() - last).count(); // ticks
    if (last.time_since_epoch().count() > 0 && age > 0 && age < kDayTicks) { // skip unset/bogus timestamps
        const uint64_t ageMs = static_cast<uint64_t>(age / kMsTicks);
        t.stamp = ageMs < nowMs ? nowMs - ageMs : 1; // backdate stamp so extrapolation catches up
    }
}

WF::IAsyncAction ReadThumb(SS::IRandomAccessStreamReference thumb,
                               std::shared_ptr<std::vector<uint8_t>> out) { // shared_ptr survives coroutine suspends
    auto stream = co_await thumb.OpenReadAsync();
    const uint64_t size = stream.Size(); // may be 0 for unknown-length streams
    if (size > kMaxCover) {
        stream.Close();
        co_return;
    }
    SS::DataReader reader(stream);
    out->reserve(static_cast<size_t>(size));
    while (out->size() < kMaxCover) {
        const uint32_t want = size ? static_cast<uint32_t>(size - std::min<uint64_t>(size, out->size())) : kChunk; // rest or a chunk
        if (!want) break;
        const uint32_t got = co_await reader.LoadAsync(want); // 0 = eof
        if (!got) break;
        const size_t at = out->size();
        out->resize(at + got);
        reader.ReadBytes(winrt::array_view<uint8_t>(out->data() + at, out->data() + at + got)); // copy into tail
    }
    reader.Close();
}

winrt::fire_and_forget ArmLoop(MediaP s);

WF::IAsyncAction Refresh(MediaP s) { // pull full state, publish if changed
    Manager mgr{nullptr};
    std::shared_ptr<const std::vector<uint8_t>> oldCover;
    uint64_t oldCoverId = 0;
    {
        std::lock_guard lock(s->mu); // copy out, never hold lock across co_await
        mgr = s->manager;
        oldCover = s->track.cover;
        oldCoverId = s->track.coverId;
    }
    if (!mgr) co_return;

    Session sel{nullptr};
    try {
        sel = mgr.GetCurrentSession();
    } catch (...) {
        LogErr(L"current session");
    }
    if (!sel) { // fallback: first session
        auto all = mgr.GetSessions();
        if (all.Size() > 0) sel = all.GetAt(0);
    }
    Hook(s, sel);

    Track t;
    std::wstring selId;
    SS::IRandomAccessStreamReference thumb{nullptr};
    const bool has = static_cast<bool>(sel);
    if (sel) {
        selId = ToW(sel.SourceAppUserModelId()); // raw id, compared to detect session switch
        t.app = AppName(selId);
        try { // each read guarded separately so one failure keeps the rest
            if (auto props = co_await sel.TryGetMediaPropertiesAsync()) {
                t.title = ToW(props.Title());
                t.artist = ToW(props.Artist());
                t.album = ToW(props.AlbumTitle());
                thumb = props.Thumbnail(); // null for many browsers
            }
        } catch (...) {
            LogErr(L"media properties");
        }
        try {
            ReadPb(t, sel.GetPlaybackInfo());
        } catch (...) {
            LogErr(L"playback info");
        }
        const uint64_t nowMs = GetTickCount64();
        try {
            ReadTl(t, sel.GetTimelineProperties(), nowMs);
        } catch (...) {
            t.stamp = nowMs; // ReadTl may throw before setting it
            LogErr(L"timeline");
        }
        if (t.duration <= 0) { // firefox-based browsers blank the timeline ~1s after each real report: keep the last real one running
            std::lock_guard lock(s->mu);
            const Track& old = s->track;
            if (s->has && selId == s->selId && old.duration > 0 && old.title == t.title && old.artist == t.artist) {
                t.position = ElapsedAt(old, nowMs);
                t.duration = old.duration;
                t.stamp = nowMs;
            }
        }
        t.canSeek = t.canSeek && t.duration > 0; // nothing to seek against until the app reports a length

        std::wstring key = t.app + L'\x1f' + t.title + L'\x1f' + t.artist + L'\x1f' + t.album; // unit separator, can't clash with text
        const bool newKey = key != s->coverKey;
        if (newKey) {
            s->coverKey = std::move(key);
            s->keyTick = nowMs;
        }
        t.cover = newKey ? nullptr : oldCover; // drop stale art on track change
        const bool want = thumb && (newKey || !oldCover || nowMs - s->keyTick < kCoverRetry); // apps often send art late
        if (want) {
            auto bytes = std::make_shared<std::vector<uint8_t>>();
            try {
                co_await ReadThumb(thumb, bytes);
            } catch (...) {
                bytes->clear(); // partial read is useless
                LogErr(L"thumbnail");
            }
            if (!bytes->empty()) {
                if (oldCover && *oldCover == *bytes) t.cover = oldCover; // reuse ptr so coverId stays stable
                else t.cover = std::move(bytes);
            }
        }
    } else {
        s->coverKey.clear(); // next session always counts as new
    }
    t.coverId = t.cover == oldCover ? oldCoverId : ++s->seq; // new id lets ui re-decode only on change

    bool changed = false, rearm = false;
    {
        std::lock_guard lock(s->mu);
        if (s->closed.load()) co_return;
        if (s->loop && selId != s->loopSel) s->loop = false; // loop belongs to the session it was turned on for
        if (!t.canRep && t.canSeek) { // no repeat of its own: we loop it by seeking back
            t.canRep = true;
            t.repeat = s->loop ? 1 : 0;
        }
        if (!t.canPrev && t.canSeek) t.canPrev = true; // no previous of its own: restart the track, like music players
        changed = !s->sent || has != s->has || selId != s->selId || !SameTrack(s->track, t); // skip redundant repaints
        if (changed) {
            s->track = std::move(t);
            s->selId = std::move(selId);
            s->has = has;
            s->sent = true;
        }
        rearm = s->loop && s->has;
    }
    if (changed) Ping(*s); // ping outside the lock
    if (rearm) ArmLoop(s); // timeline may have moved, reschedule the restart
}

winrt::fire_and_forget Worker(MediaP s) { // single refresh loop, holds a strong ref
    try {
        co_await winrt::resume_after(kDebounce); // let the event burst settle
        for (;;) {
            while (!s->closed.load() && s->pending.exchange(false)) { // coalesce pokes into one refresh
                try {
                    co_await Refresh(s);
                } catch (...) {
                    LogErr(L"refresh");
                }
            }
            s->running.store(false);
            if (s->closed.load() || !s->pending.load() || s->running.exchange(true)) break; // recheck: poke may race the store
        }
    } catch (...) {
        s->running.store(false);
        LogErr(L"worker");
    }
}

void Poke(const MediaP& s) noexcept { // request a refresh, start worker if idle
    if (s->closed.load()) return;
    s->pending.store(true);
    if (s->running.exchange(true)) return; // worker already active, it will see pending
    try {
        Worker(s);
    } catch (...) {
        s->running.store(false);
        LogErr(L"start worker");
    }
}

winrt::fire_and_forget StartMedia(MediaP s) {
    try {
        co_await winrt::resume_background(); // RequestAsync must not block the ui thread
        Manager mgr = co_await Manager::RequestAsync();
        if (!mgr) co_return;
        std::weak_ptr<MediaSt> w = s;
        auto r1 = mgr.SessionsChanged(winrt::auto_revoke, [w](auto&&, auto&&) { KickSettle(w); });
        auto r2 = mgr.CurrentSessionChanged(winrt::auto_revoke, [w](auto&&, auto&&) { KickSettle(w); });
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load()) co_return; // closed meanwhile, revokers unhook on return
            s->manager = mgr;
            s->listRev = std::move(r1);
            s->curRev = std::move(r2);
        }
        Poke(s); // initial read
    } catch (...) {
        LogErr(L"start");
    }
}

WF::IAsyncOperation<bool> SeekTicks(MediaP s, Session sel, int64_t rel) { // rel = ticks from track start
    const auto tl = sel.GetTimelineProperties();
    const int64_t start = tl && tl.EndTime().count() > tl.StartTime().count() ? tl.StartTime().count() : 0; // blanked timeline starts at 0
    const bool ok = co_await sel.TryChangePlaybackPositionAsync(start + rel); // absolute, so add start back
    if (ok) {
        bool post = false;
        {
            std::lock_guard lock(s->mu);
            if (!s->closed.load() && s->has && s->track.duration > 0) {
                s->track.position = std::min(rel, s->track.duration); // optimistic update, app event lags
                s->track.stamp = GetTickCount64();
                post = true;
            }
        }
        if (post) Ping(*s);
    }
    co_return ok;
}

double LeftMs(const Track& t) { // real time until the track ends, at its playback rate
    return static_cast<double>(t.duration - ElapsedAt(t, GetTickCount64())) / kMsTicks / t.rate;
}

winrt::fire_and_forget ArmLoop(MediaP s) { // one pending restart per loop cycle, a newer arm cancels the older
    try {
        uint64_t gen = 0, waitMs = 0;
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load() || !s->loop || !s->has || s->track.duration <= 0) co_return;
            const double left = LeftMs(s->track);
            if (!s->track.playing && left > kLoopEndMs) { // paused by the user: nothing pending, play re-arms
                ++s->loopGen;
                s->loopDue = 0;
                co_return;
            }
            waitMs = left > kLoopLeadMs ? static_cast<uint64_t>(left) - kLoopLeadMs : 0;
            const uint64_t due = GetTickCount64() + waitMs;
            const uint64_t drift = due > s->loopDue ? due - s->loopDue : s->loopDue - due;
            if (s->loopDue && drift < kLoopSlackMs) co_return; // pending restart still on time, don't stack another
            gen = ++s->loopGen;
            s->loopDue = due;
        }
        co_await winrt::resume_after(std::chrono::milliseconds(waitMs));
        Session sel{nullptr};
        bool replay = false, early = false;
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load() || !s->loop || gen != s->loopGen || !s->has) co_return; // turned off or re-armed meanwhile
            s->loopDue = 0;
            const double left = LeftMs(s->track);
            if (!s->track.playing && left > kLoopEndMs) co_return; // paused by the user while we waited: leave it
            early = left > 2.0 * kLoopLeadMs; // timeline moved while we waited
            replay = !s->track.playing; // ran to the end and stopped
            sel = s->session;
        }
        if (early) {
            ArmLoop(s);
            co_return;
        }
        if (!sel || !co_await SeekTicks(s, sel, 0)) co_return;
        if (replay) co_await sel.TryPlayAsync();
        ArmLoop(s); // next cycle
    } catch (...) {
        LogErr(L"loop");
    }
}

enum class Cmd { TogglePlay, Next, Previous, Seek, Shuffle, Repeat };

winrt::fire_and_forget RunCmd(MediaP s, Cmd cmd, double arg) { // by value: keeps state alive across awaits
    try {
        co_await winrt::resume_background(); // ui thread returns immediately
        Session sel{nullptr};
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load()) co_return;
            sel = s->session; // act on the session the ui is showing
        }
        if (!sel) co_return;
        bool ok = false;
        switch (cmd) {
            case Cmd::TogglePlay: {
                ok = co_await sel.TryTogglePlayPauseAsync();
                if (!ok) { // some apps reject toggle but accept play/pause
                    const auto pb = sel.GetPlaybackInfo();
                    const bool playing = pb && pb.PlaybackStatus() ==
                        MC::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                    ok = playing ? co_await sel.TryPauseAsync() : co_await sel.TryPlayAsync();
                }
                break;
            }
            case Cmd::Next: ok = co_await sel.TrySkipNextAsync(); break;
            case Cmd::Previous: {
                const auto pb = sel.GetPlaybackInfo();
                if (pb && pb.Controls() && pb.Controls().IsPreviousEnabled()) ok = co_await sel.TrySkipPreviousAsync();
                else ok = co_await SeekTicks(s, sel, 0); // no previous of its own: restart the track
                break;
            }
            case Cmd::Seek: {
                const auto tl = sel.GetTimelineProperties(); // fresh read, cached track may be stale
                int64_t dur = tl ? tl.EndTime().count() - tl.StartTime().count() : 0; // ticks
                if (dur <= 0) { // timeline blanked between reports, use the last real length
                    std::lock_guard lock(s->mu);
                    dur = s->track.duration;
                }
                if (dur <= 0) break; // unknown length, can't map fraction
                const double f = std::clamp(arg, 0.0, 1.0);
                ok = co_await SeekTicks(s, sel, static_cast<int64_t>(f * static_cast<double>(dur)));
                break;
            }
            case Cmd::Shuffle: {
                const auto pb = sel.GetPlaybackInfo();
                bool cur = false;
                if (pb)
                    if (auto v = pb.IsShuffleActive()) cur = v.Value(); // null treated as off
                ok = co_await sel.TryChangeShuffleActiveAsync(!cur);
                break;
            }
            case Cmd::Repeat: {
                const auto pb = sel.GetPlaybackInfo();
                if (!(pb && pb.Controls() && pb.Controls().IsRepeatEnabled())) { // no repeat of its own: we loop it
                    bool on = false;
                    {
                        std::lock_guard lock(s->mu);
                        if (s->closed.load()) co_return;
                        on = s->loop = !s->loop;
                        s->loopSel = s->selId;
                        ++s->loopGen; // cancel any pending restart
                        s->loopDue = 0;
                        if (s->has && s->track.canRep) s->track.repeat = on ? 1 : 0; // show it right away
                    }
                    Ping(*s);
                    if (on) ArmLoop(s);
                    ok = true;
                    break;
                }
                RepeatMode cur = RepeatMode::None;
                if (pb)
                    if (auto v = pb.AutoRepeatMode()) cur = v.Value();
                const RepeatMode next = cur == RepeatMode::None ? RepeatMode::List // cycle none -> list -> track
                                      : cur == RepeatMode::List ? RepeatMode::Track
                                                                : RepeatMode::None;
                ok = co_await sel.TryChangeAutoRepeatModeAsync(next);
                break;
            }
        }
        if (!ok) Log(L"command", L"not accepted by the session"); // app refused, events will resync ui
    } catch (...) {
        LogErr(L"command");
    }
}

struct RadioState {
    std::atomic<HWND> notify{nullptr};
    std::atomic<UINT> msg{0};
    std::atomic<bool> closed{false}, started{false};
    std::atomic<bool> hasWifi{false}, hasBt{false}, wifiOn{false}, btOn{false}; // atomics so getters skip the lock

    std::mutex mu; // guards radios + revokers
    WR::Radio wifi{nullptr}, bt{nullptr};
    WR::Radio::StateChanged_revoker wifiRev, btRev;
};
using RadioP = std::shared_ptr<RadioState>;

void OnRadio(const std::weak_ptr<RadioState>& w, const WR::Radio& sender) noexcept { // runs on winrt threadpool
    try {
        auto s = w.lock();
        if (!s || s->closed.load() || !sender) return;
        const bool on = sender.State() == WR::RadioState::On;
        const auto kind = sender.Kind();
        std::atomic<bool>* slot = kind == WR::RadioKind::WiFi ? &s->wifiOn // pick flag by radio kind
                                : kind == WR::RadioKind::Bluetooth ? &s->btOn
                                                                   : nullptr;
        if (slot && slot->exchange(on) != on) Ping(*s); // ping only on real flips
    } catch (...) {
        LogErr(L"radio changed");
    }
}

winrt::fire_and_forget StartRadios(RadioP s) {
    try {
        co_await winrt::resume_background();
        const auto access = co_await WR::Radio::RequestAccessAsync(); // needed for SetStateAsync
        if (access != WR::RadioAccessStatus::Allowed) Log(L"radios", L"access not allowed; states are read-only");
        const auto radios = co_await WR::Radio::GetRadiosAsync();
        WR::Radio wifi{nullptr}, bt{nullptr};
        for (const auto& r : radios) {
            const auto kind = r.Kind();
            if (kind == WR::RadioKind::WiFi && !wifi) wifi = r; // first of each kind wins
            else if (kind == WR::RadioKind::Bluetooth && !bt) bt = r;
        }
        std::weak_ptr<RadioState> w = s;
        WR::Radio::StateChanged_revoker wr, br;
        if (wifi) wr = wifi.StateChanged(winrt::auto_revoke, [w](const WR::Radio& r, auto&&) { OnRadio(w, r); });
        if (bt) br = bt.StateChanged(winrt::auto_revoke, [w](const WR::Radio& r, auto&&) { OnRadio(w, r); });
        const bool wifiOn = wifi && wifi.State() == WR::RadioState::On; // read after subscribing so no flip is missed
        const bool btOn = bt && bt.State() == WR::RadioState::On;
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load()) co_return; // revokers unhook on return
            s->wifi = wifi;
            s->bt = bt;
            s->wifiRev = std::move(wr);
            s->btRev = std::move(br);
            s->hasWifi.store(static_cast<bool>(wifi));
            s->hasBt.store(static_cast<bool>(bt));
            s->wifiOn.store(wifiOn);
            s->btOn.store(btOn);
        }
        Ping(*s); // first paint with real state
    } catch (...) {
        LogErr(L"radios start");
    }
}

winrt::fire_and_forget SetRadio(RadioP s, bool wifiRadio, bool on) {
    try {
        co_await winrt::resume_background();
        WR::Radio r{nullptr};
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load()) co_return;
            r = wifiRadio ? s->wifi : s->bt; // copy ref, await unlocked
        }
        if (!r) co_return;
        const auto res = co_await r.SetStateAsync(on ? WR::RadioState::On : WR::RadioState::Off); // StateChanged reports the result
        if (res != WR::RadioAccessStatus::Allowed) Log(L"radios", L"SetStateAsync was not allowed");
    } catch (...) {
        LogErr(L"radios set");
    }
}

void CloseRadios(const RadioP& s) noexcept {
    try {
        WR::Radio::StateChanged_revoker a, b; // released after lock scope
        WR::Radio wifi{nullptr}, bt{nullptr};
        {
            std::lock_guard lock(s->mu);
            s->closed.store(true); // stops late callbacks from pinging
            a = std::move(s->wifiRev);
            b = std::move(s->btRev);
            wifi = std::move(s->wifi);
            bt = std::move(s->bt);
        } // radios + revokers released unlocked
    } catch (...) {
        LogErr(L"radios close");
    }
}

}

int64_t Track::Elapsed() const { return ElapsedAt(*this, GetTickCount64()); }

double Track::Progress() const {
    if (duration <= 0) return 0.0; // unknown length, avoid div by zero
    return std::clamp(static_cast<double>(Elapsed()) / static_cast<double>(duration), 0.0, 1.0);
}

struct Media::Impl {
    MediaP s; // shared so in-flight coroutines outlive Media
};

Media::Media() {
    try {
        impl = std::make_unique<Impl>();
        impl->s = std::make_shared<MediaSt>();
    } catch (...) {
        impl.reset(); // null impl = every call is a no-op
        LogErr(L"Media()");
    }
}

Media::~Media() {
    if (impl && impl->s) CloseMedia(impl->s); // coroutines see closed and bail
}

void Media::Start(HWND notify, UINT msg) {
    if (!impl || !impl->s) return;
    auto& s = impl->s;
    s->notify.store(notify); // re-targeting allowed on repeat calls
    s->msg.store(msg);
    if (s->started.exchange(true)) return; // start once
    try {
        StartMedia(s); // returns at first co_await
    } catch (...) {
        LogErr(L"Media::Start");
    }
}

bool Media::Snapshot(Track& track) const {
    try {
        if (!impl || !impl->s) {
            track = Track{};
            return false;
        }
        const auto& s = impl->s;
        std::lock_guard lock(s->mu);
        track = s->track; // cheap copy, cover is a shared_ptr
        return s->has;
    } catch (...) {
        LogErr(L"Media::Snapshot");
        return false;
    }
}

void Media::TogglePlay() { if (impl && impl->s) try { RunCmd(impl->s, Cmd::TogglePlay, 0); } catch (...) {} } // swallow: ui must not throw
void Media::Next() { if (impl && impl->s) try { RunCmd(impl->s, Cmd::Next, 0); } catch (...) {} }
void Media::Previous() { if (impl && impl->s) try { RunCmd(impl->s, Cmd::Previous, 0); } catch (...) {} }
void Media::Seek(double fraction) {
    if (!(fraction >= 0.0)) fraction = 0.0; // negated form also catches nan
    if (impl && impl->s) try { RunCmd(impl->s, Cmd::Seek, fraction); } catch (...) {}
}
void Media::ToggleShuffle() { if (impl && impl->s) try { RunCmd(impl->s, Cmd::Shuffle, 0); } catch (...) {} }
void Media::CycleRepeat() { if (impl && impl->s) try { RunCmd(impl->s, Cmd::Repeat, 0); } catch (...) {} }

struct Radios::Impl {
    RadioP s; // shared with async start/set
};

Radios::Radios() {
    try {
        impl = std::make_unique<Impl>();
        impl->s = std::make_shared<RadioState>();
    } catch (...) {
        impl.reset(); // degrade to no radios
        LogErr(L"Radios()");
    }
}

Radios::~Radios() {
    if (impl && impl->s) CloseRadios(impl->s); // unhook before window dies
}

void Radios::Start(HWND notify, UINT msg) {
    if (!impl || !impl->s) return;
    auto& s = impl->s;
    s->notify.store(notify);
    s->msg.store(msg);
    if (s->started.exchange(true)) return; // idempotent
    try {
        StartRadios(s); // fire and forget
    } catch (...) {
        LogErr(L"Radios::Start");
    }
}

bool Radios::HasWifi() const { return impl && impl->s && impl->s->hasWifi.load(); } // lock-free reads for paint
bool Radios::HasBt() const { return impl && impl->s && impl->s->hasBt.load(); }
bool Radios::WifiOn() const { return impl && impl->s && impl->s->wifiOn.load(); }
bool Radios::BtOn() const { return impl && impl->s && impl->s->btOn.load(); }

void Radios::SetWifi(bool on) {
    if (impl && impl->s) try { SetRadio(impl->s, true, on); } catch (...) {} // ui flips when StateChanged fires
}
void Radios::SetBt(bool on) {
    if (impl && impl->s) try { SetRadio(impl->s, false, on); } catch (...) {}
}

}
