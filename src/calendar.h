#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The user's own calendars, from their private calendar links (google's "secret address in ical format", outlook's and
// icloud's published links). The links are kept encrypted for this windows user in %LOCALAPPDATA%\Astraeus\accounts,
// shared by astraeus island and halobar: connect in either, both show it. Same file in both apps.
namespace sys::cal {

struct Event {
    int64_t start = 0, end = 0; // unix seconds; an all-day event spans local midnight to midnight
    bool allDay = false;
    std::wstring title, where;
};

struct Link {
    std::wstring url, name; // name: which service it came from, for the list of connected calendars
};

class Calendars {
public:
    Calendars();
    ~Calendars();
    Calendars(const Calendars&) = delete;
    Calendars& operator=(const Calendars&) = delete;

    void Sync(HWND notify, UINT msg, int maxAge); // fetches on a worker if the last fetch is older than maxAge seconds (0: now); msg is posted when events change
    bool Linked() const; // at least one calendar is connected
    bool Busy() const;
    std::vector<Link> Links() const;
    std::vector<Event> Day(int y, int m, int d) const; // that local day's events, all-day first, then by start
    bool HasEvents(int y, int m, int d) const;
    bool Add(const std::wstring& url, HWND notify, UINT msg); // a link the user copied: saved, encrypted, and fetched
    void Remove(size_t i, HWND notify, UINT msg);
    static bool LooksLikeCalendar(const std::wstring& text, std::wstring* url = nullptr); // a calendar link, cleaned up
    static const wchar_t* GoogleSettings() { return L"https://calendar.google.com/calendar/r/settings"; } // where the secret address lives
    static std::wstring NewEventUrl(int y, int m, int d); // google's own new-event page for that day: saving there puts it in the user's account
    static constexpr int kEvery = 5 * 60, kOnOpen = 60; // background re-check, and the most a just-opened calendar may be behind

private:
    struct Impl;
    std::shared_ptr<Impl> impl; // shared with the worker, so a late finish never touches a freed object
};

} // namespace sys::cal
