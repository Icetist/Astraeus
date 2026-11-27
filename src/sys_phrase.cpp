// The spoken clip phrase. Windows' speech recognizer is given a list of exactly one phrase, so it isn't transcribing
// anything: it only answers whether what it heard was that phrase, and how sure it is. It runs on this pc, with no
// internet, and opens the microphone in shared mode the way voice chat apps do, so they all keep it at the same time.
// Silence costs next to nothing: the engine wakes only when someone speaks. A session Windows ends (a long silence,
// the microphone unplugged) is started again.
#include "sys_phrase.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.SpeechRecognition.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sys::phrase {

struct Listener::Impl {
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false, ended = false, on = false;
    std::wstring phrase, why;
};

namespace {

using namespace winrt::Windows::Media::SpeechRecognition;

constexpr auto kEndSilence = std::chrono::milliseconds(500); // shorter cut the phrase off mid-word: most tries came back rejected
constexpr auto kStayOn = std::chrono::hours(24); // a session listens this long before Windows ends it, then it starts again
constexpr auto kRetry = std::chrono::seconds(2); // a session that ended badly waits this long before trying again
constexpr HRESULT kPrivacy = (HRESULT)0x80045509; // speech privacy policy not accepted

std::wstring Trimmed(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}

void Run(std::shared_ptr<Listener::Impl> s, std::wstring phrase, HWND notify, UINT msg) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        SpeechRecognizer rec;
        auto words = winrt::single_threaded_vector<winrt::hstring>({winrt::hstring(phrase)});
        rec.Constraints().Append(SpeechRecognitionListConstraint(words, L"clip"));
        SpeechRecognitionCompilationResult compiled = rec.CompileConstraintsAsync().get();
        if (compiled.Status() != SpeechRecognitionResultStatus::Success) {
            std::lock_guard<std::mutex> lock(s->mu);
            s->why = L"Windows' speech recognition isn't set up for your language";
            return;
        }
        rec.Timeouts().EndSilenceTimeout(kEndSilence);
        SpeechContinuousRecognitionSession session = rec.ContinuousRecognitionSession();
        session.AutoStopSilenceTimeout(kStayOn);
        auto heard = session.ResultGenerated(winrt::auto_revoke, [notify, msg](auto&&, SpeechContinuousRecognitionResultGeneratedEventArgs const& a) {
            SpeechRecognitionResult r = a.Result();
            // a made-up word like "astra" never scores above low, even said clearly: other speech comes back rejected
            if (r.Status() == SpeechRecognitionResultStatus::Success && r.Confidence() != SpeechRecognitionConfidence::Rejected && !r.Text().empty())
                PostMessageW(notify, msg, 0, 0);
        });
        auto over = session.Completed(winrt::auto_revoke, [s](auto&&, auto&&) {
            std::lock_guard<std::mutex> lock(s->mu);
            s->ended = true;
            s->cv.notify_all();
        });
        session.StartAsync().get();
        {
            std::lock_guard<std::mutex> lock(s->mu);
            s->on = true;
        }
        std::unique_lock<std::mutex> lock(s->mu);
        while (!s->stop) {
            s->cv.wait(lock, [&] { return s->stop || s->ended; });
            if (s->stop) break;
            s->ended = false;
            lock.unlock();
            std::this_thread::sleep_for(kRetry); // windows ended it: listening again, unhurried
            try {
                session.StartAsync().get();
            } catch (...) { // the microphone may be gone for now: tried again after the wait
                std::lock_guard<std::mutex> relock(s->mu);
                s->ended = true;
            }
            lock.lock();
        }
        lock.unlock();
        try {
            session.StopAsync().get();
        } catch (...) {
        }
    } catch (winrt::hresult_error const& e) {
        std::lock_guard<std::mutex> lock(s->mu);
        HRESULT hr = e.code();
        s->why = hr == kPrivacy ? L"Turn on speech recognition in Windows Settings › Privacy › Speech"
                 : hr == E_ACCESSDENIED ? L"Let desktop apps use your microphone in Windows Settings › Privacy › Microphone"
                                        : L"Your microphone couldn't be listened to";
    }
    std::lock_guard<std::mutex> lock(s->mu);
    s->on = false;
}

}

Listener::Listener() = default;

Listener::~Listener() { Stop(); }

void Listener::Start(const std::wstring& phrase, HWND notify, UINT msg) {
    std::wstring p = Trimmed(phrase);
    if (p.empty()) return Stop();
    if (impl) {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->phrase == p && !impl->stop) return; // already listening for it
    }
    Stop();
    impl = std::make_shared<Impl>();
    impl->phrase = p;
    std::thread(Run, impl, p, notify, msg).detach(); // the engine starts in a moment or two: the ui doesn't wait
}

void Listener::Stop() {
    if (!impl) return;
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        impl->stop = true;
    }
    impl->cv.notify_all();
    impl.reset(); // the thread keeps its own share until it has let go of the microphone
}

bool Listener::On() const {
    if (!impl) return false;
    std::lock_guard<std::mutex> lock(impl->mu);
    return impl->on;
}

std::wstring Listener::Phrase() const {
    if (!impl) return L"";
    std::lock_guard<std::mutex> lock(impl->mu);
    return impl->phrase;
}

std::wstring Listener::Why() const {
    if (!impl) return L"";
    std::lock_guard<std::mutex> lock(impl->mu);
    return impl->why;
}

}
