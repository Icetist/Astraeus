#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// What the widgets read from outside: quotes, rates, the sky's details, the thermal zone, screen time, and the accounts people connect with their own keys.
// A thread runs only while a widget in sight wants something, each feed at its own slow pace; nothing is fetched for a widget that isn't there.
namespace feeds {

struct Series { // one feed's latest
    bool ok = false; // a good answer has come
    std::wstring error; // why not, in words, when it hasn't
    double value = 0, total = 0, week = 0, change = 0; // the headline number; a 30 day sum; the last 7 days; the share they moved against the 7 before
    double extra = 0, extra2 = 0; // a second and third number, the feed says which
    std::vector<double> points; // oldest first
    std::wstring text; // words, when the answer has some
    std::vector<std::wstring> rows; // lines of a list
    std::wstring name; // what it's called, when the answer says: a company's or an index's name
    std::vector<double> more; // further numbers, the feed says which: a quote's are QuoteMore
};

enum QuoteMore { kPrevClose, kDayHigh, kDayLow, kYearHigh, kYearLow, kVolume, kOpensAt, kClosesAt, kPreAt, kPostAt, kQuoteMore }; // a quote's further numbers; the times are unix seconds
enum Phase { kClosed, kPreMarket, kOpen, kAfterHours }; // where its market's day is
Phase MarketPhase(const Series& quote); // from the sessions the quote came with, against the clock now
const wchar_t* PhaseName(Phase p);

void Start(HWND notify, UINT msg, const wchar_t* folder); // msg arrives when a feed has something new; folder: under %localappdata%, for the screen time log
void Want(const std::vector<std::string>& ids); // what's in sight now; an empty list ends the thread
void Refresh(const std::string& id); // asked for by hand, or its settings changed
void Stop();
Series Get(const std::string& id);

std::wstring Setting(const char* key); // plain settings, under the app's own key
void SetSetting(const char* key, const std::wstring& value);
std::wstring Secret(const char* key); // a key or token: encrypted for this windows user, never written in the clear
void SetSecret(const char* key, const std::wstring& value);

struct Used { // today's or the week's time in one app
    std::wstring app;
    int seconds = 0;
};
struct Screen {
    int today = 0, week = 0; // seconds
    int hours[24] = {}; // today's seconds by hour
    int days[7] = {}; // the last seven days, oldest first
    std::vector<Used> apps; // today's, longest first
    std::vector<Used> appsWeek;
};
Screen ScreenTime();

}
