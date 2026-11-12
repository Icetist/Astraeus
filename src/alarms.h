#pragma once
// Persistent alarms, 1 Hz scheduling, snooze, and sleep missed-alarm tracking.
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

struct Alarm {
    int id = 0;
    int hour = 8;
    int minute = 0;
    bool daily = false;
    bool enabled = true;
    bool missed = false;
    int64_t snoozeUntil = 0;
    std::wstring label;
};

int64_t LocalEpochSec(const SYSTEMTIME& st); // wall-clock seconds via SystemTimeToFileTime

struct AlarmStore {
    static constexpr int kMaxAlarms = 16;
    static constexpr int kSnoozeSec = 5 * 60;
    static constexpr size_t kMaxLabelLen = 40;

    std::vector<Alarm> items;
    int nextId = 1;
    int ringingId = 0;
    int64_t ringStarted = 0;
    int64_t lastCheck = 0;

    static std::wstring SanitizeLabel(const std::wstring& s);
    static std::wstring FormatTime(int hour, int minute);

    std::wstring Serialize() const;
    void Deserialize(const std::wstring& raw);

    int Add(int hour, int minute, bool daily, const std::wstring& label);
    bool Update(int id, int hour, int minute, bool daily, const std::wstring& label);
    bool Remove(int id);
    bool Toggle(int id);
    void ClearMissed();
    bool HasMissed() const;

    int Tick(int64_t nowLocalSec, const SYSTEMTIME& localNow);
    bool Snooze(int64_t nowLocalSec);
    bool Dismiss();
    const Alarm* Ringing() const;
    const Alarm* Find(int id) const;
};
