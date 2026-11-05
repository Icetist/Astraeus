#pragma once
// a typed timer length: 20 is minutes, or 1h 30m, 1h30, 1.5h, 90s, or a clock like the countdown's own, 1:30 and 1:30:00
#include <string>

namespace duration {

constexpr int kMaxSec = 24 * 60 * 60; // a day: the longest countdown still fits its place on the page
constexpr size_t kMaxChars = 24;

inline bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

inline int FromClock(const std::wstring& s) { // m:ss or h:mm:ss, read the way the countdown shows them
    long long total = 0;
    int parts = 0, digits = 0, part = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && Digit(s[i])) {
            part = part * 10 + (s[i] - L'0');
            if (++digits > 4) return 0;
            continue;
        }
        if (i < s.size() && s[i] != L':') return 0;
        if (!digits || (parts > 0 && (digits != 2 || part >= 60))) return 0; // only the first part may run long
        total = total * 60 + part;
        ++parts, digits = 0, part = 0;
    }
    return parts >= 2 && parts <= 3 && total <= kMaxSec ? (int)total : 0;
}

inline int UnitOf(const std::wstring& w) { // seconds in a unit word, 0 when it isn't one
    for (const wchar_t* n : {L"h", L"hr", L"hrs", L"hour", L"hours"}) if (w == n) return 3600;
    for (const wchar_t* n : {L"m", L"min", L"mins", L"minute", L"minutes"}) if (w == n) return 60;
    for (const wchar_t* n : {L"s", L"sec", L"secs", L"second", L"seconds"}) if (w == n) return 1;
    return 0;
}

inline int Parse(const std::wstring& typed) { // seconds, 0 when it isn't a time
    if (typed.size() > kMaxChars) return 0;
    std::wstring s;
    for (wchar_t c : typed) s += c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c;
    size_t i = s.find_first_not_of(L' ');
    if (i == std::wstring::npos) return 0;
    if (s.find(L':') != std::wstring::npos) return FromClock(s.substr(i, s.find_last_not_of(L' ') - i + 1));
    double total = 0;
    int last = 0; // the unit before: each one must be smaller, and a bare number takes the next one down
    while (i < s.size()) {
        if (!Digit(s[i])) return 0;
        double v = 0, place = 1;
        for (; i < s.size() && Digit(s[i]); ++i) v = v * 10 + (s[i] - L'0');
        if (i < s.size() && s[i] == L'.') {
            if (++i >= s.size() || !Digit(s[i])) return 0;
            for (; i < s.size() && Digit(s[i]); ++i) v += (s[i] - L'0') * (place /= 10);
        }
        while (i < s.size() && s[i] == L' ') ++i;
        std::wstring word;
        while (i < s.size() && s[i] >= L'a' && s[i] <= L'z') word += s[i++];
        while (i < s.size() && s[i] == L' ') ++i;
        int unit = !word.empty() ? UnitOf(word) : last == 0 || last == 3600 ? 60 : last == 60 ? 1 : 0;
        if (!unit || (last && unit >= last)) return 0;
        total += v * unit, last = unit;
    }
    total += 0.5; // the nearest second
    return total >= 1 && total < kMaxSec + 1 ? (int)total : 0;
}

}
