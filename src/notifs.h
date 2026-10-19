#pragma once
#include <cstdint>
#include <string>
#include <vector>

// action center toasts, read straight from windows' own notification db (wpndatabase.db). nothing is held open while idle.
namespace sys {

struct Notif {
    int64_t id = 0;  // Notification.Id, stable per toast
    std::wstring app;  // friendly app name
    std::wstring aumid;  // NotificationHandler.PrimaryId
    std::wstring title, body;  // first <text>, the rest joined with \n
    int64_t arrived = 0;  // FILETIME (100 ns since 1601, utc)
    std::wstring launch;  // toast launch attribute, may be empty
    std::wstring image;  // appLogoOverride as a local file path, else empty
};

std::vector<Notif> ReadNotifs(size_t max = 50) noexcept;  // newest first, toasts only, not expired; empty on any failure
int64_t NotifStamp() noexcept;  // changes when the db or its wal is written; cheap, poll only while the panel is open
bool OpenNotifApp(const Notif& n) noexcept;  // protocol launch uri if it looks like one, else shell:AppsFolder\<aumid>
std::wstring AppNameOf(const std::wstring& aumid) noexcept;  // shell display name, cached; falls back to the aumid's last segment

// "do not disturb" as the per-user banner master switch (PushNotifications\ToastEnabled), see sys_notifs.cpp for scope
bool SetDoNotDisturb(bool on) noexcept;
bool DoNotDisturb() noexcept;

}  // namespace sys
