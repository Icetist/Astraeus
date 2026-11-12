#include "alarms.h"

#include <algorithm>
#include <cstdio>

namespace {

int ClampHour(int h) { return (std::clamp)(h, 0, 23); }
int ClampMinute(int m) { return (std::clamp)(m, 0, 59); }

void SortAlarms(std::vector<Alarm>& items) {
    std::stable_sort(items.begin(), items.end(), [](const Alarm& a, const Alarm& b) {
        int ma = a.hour * 60 + a.minute, mb = b.hour * 60 + b.minute;
        return ma != mb ? ma < mb : a.id < b.id;
    });
}

} // namespace

int64_t LocalEpochSec(const SYSTEMTIME& st) {
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(ui.QuadPart / 10000000ULL);
}

std::wstring AlarmStore::SanitizeLabel(const std::wstring& s) {
    std::wstring cleaned;
    cleaned.reserve((std::min)(s.size(), kMaxLabelLen));
    for (wchar_t ch : s) {
        if (ch < 32 || ch == 127 || ch == L'|' || ch == L';' || ch == L'"') continue;
        cleaned.push_back(ch);
    }
    size_t a = cleaned.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return {};
    size_t b = cleaned.find_last_not_of(L" \t");
    cleaned = cleaned.substr(a, b - a + 1);
    if (cleaned.size() > kMaxLabelLen) cleaned.resize(kMaxLabelLen);
    return cleaned;
}

std::wstring AlarmStore::FormatTime(int hour, int minute) {
    wchar_t buf[8]{};
    swprintf_s(buf, L"%02d:%02d", ClampHour(hour), ClampMinute(minute));
    return buf;
}

std::wstring AlarmStore::Serialize() const {
    std::wstring out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out.push_back(L';');
        const Alarm& a = items[i];
        out += std::to_wstring(ClampHour(a.hour)) + L'|';
        out += std::to_wstring(ClampMinute(a.minute)) + L'|';
        out += (a.daily ? L"1|" : L"0|");
        out += (a.enabled ? L"1|" : L"0|");
        out += (a.missed ? L"1|" : L"0|");
        out += SanitizeLabel(a.label);
    }
    return out;
}

void AlarmStore::Deserialize(const std::wstring& raw) {
    items.clear();
    nextId = 1;
    ringingId = 0;
    ringStarted = 0;
    for (size_t at = 0; at < raw.size() && (int)items.size() < kMaxAlarms;) {
        size_t semi = raw.find(L';', at);
        if (semi == std::wstring::npos) semi = raw.size();
        std::wstring rec = raw.substr(at, semi - at);
        at = semi + 1;
        if (rec.empty()) continue;

        std::vector<std::wstring> cols;
        for (size_t p = 0; p <= rec.size() && cols.size() < 6;) {
            if (cols.size() == 5) {
                cols.push_back(rec.substr(p));
                break;
            }
            size_t bar = rec.find(L'|', p);
            if (bar == std::wstring::npos) break;
            cols.push_back(rec.substr(p, bar - p));
            p = bar + 1;
        }
        if (cols.size() < 5) continue;

        Alarm a;
        a.id = nextId++;
        try {
            a.hour = ClampHour(std::stoi(cols[0]));
            a.minute = ClampMinute(std::stoi(cols[1]));
        } catch (...) {
            continue;
        }
        a.daily = cols[2] == L"1";
        a.enabled = cols[3] == L"1";
        a.missed = cols[4] == L"1";
        if (cols.size() >= 6) a.label = SanitizeLabel(cols[5]);
        items.push_back(std::move(a));
    }
    SortAlarms(items);
}

int AlarmStore::Add(int hour, int minute, bool daily, const std::wstring& label) {
    if ((int)items.size() >= kMaxAlarms) return 0;
    Alarm a;
    a.id = nextId++;
    a.hour = ClampHour(hour);
    a.minute = ClampMinute(minute);
    a.daily = daily;
    a.enabled = true;
    a.label = SanitizeLabel(label);
    int id = a.id;
    items.push_back(std::move(a));
    SortAlarms(items);
    return id;
}

bool AlarmStore::Update(int id, int hour, int minute, bool daily, const std::wstring& label) {
    for (auto& a : items) {
        if (a.id != id) continue;
        a.hour = ClampHour(hour);
        a.minute = ClampMinute(minute);
        a.daily = daily;
        a.enabled = true;
        a.missed = false;
        a.snoozeUntil = 0;
        a.label = SanitizeLabel(label);
        if (ringingId == id) ringingId = 0, ringStarted = 0;
        SortAlarms(items);
        return true;
    }
    return false;
}

bool AlarmStore::Remove(int id) {
    auto it = std::find_if(items.begin(), items.end(), [id](const Alarm& a) { return a.id == id; });
    if (it == items.end()) return false;
    items.erase(it);
    if (ringingId == id) ringingId = 0, ringStarted = 0;
    return true;
}

bool AlarmStore::Toggle(int id) {
    for (auto& a : items) {
        if (a.id != id) continue;
        a.enabled = !a.enabled;
        a.missed = false;
        a.snoozeUntil = 0;
        if (!a.enabled && ringingId == id) ringingId = 0, ringStarted = 0;
        return true;
    }
    return false;
}

void AlarmStore::ClearMissed() {
    for (auto& a : items) a.missed = false;
}

bool AlarmStore::HasMissed() const {
    return std::any_of(items.begin(), items.end(), [](const Alarm& a) { return a.missed; });
}

int AlarmStore::Tick(int64_t nowLocalSec, const SYSTEMTIME&) {
    lastCheck = nowLocalSec;
    return 0;
}

bool AlarmStore::Snooze(int64_t) { return false; }
bool AlarmStore::Dismiss() { return false; }

const Alarm* AlarmStore::Ringing() const { return Find(ringingId); }

const Alarm* AlarmStore::Find(int id) const {
    if (id <= 0) return nullptr;
    for (const auto& a : items)
        if (a.id == id) return &a;
    return nullptr;
}
