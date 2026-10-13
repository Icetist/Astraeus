#include "sys.h"

#include <powrprof.h>
#include <shellapi.h>

namespace sys {
namespace {

constexpr DWORD kReason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED; // planned, so no shutdown tracker prompt

bool GrantShutdown() { // process token lacks shutdown privilege by default
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        OutputDebugStringW(L"[sys] OpenProcessToken failed\n");
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS; // Adjust returns true even on ERROR_NOT_ALL_ASSIGNED
    CloseHandle(token);
    if (!ok) OutputDebugStringW(L"[sys] could not enable SE_SHUTDOWN_NAME\n");
    return ok;
}

void Exit(UINT flags, bool priv) {
    if (priv) GrantShutdown(); // result ignored, ExitWindowsEx reports failure
    if (!ExitWindowsEx(flags, kReason)) OutputDebugStringW(L"[sys] ExitWindowsEx failed\n");
}

void Shell(const wchar_t* target) {
    const auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL)); // HINSTANCE is really an error code
    if (rc <= 32) OutputDebugStringW(L"[sys] ShellExecuteW failed\n"); // <= 32 means failure per docs
}

}

namespace power {

void Lock() {
    if (!LockWorkStation()) OutputDebugStringW(L"[sys] LockWorkStation failed\n");
}

void Sleep() {
    if (!SetSuspendState(FALSE, FALSE, FALSE)) OutputDebugStringW(L"[sys] SetSuspendState failed\n"); // sleep, not hibernate
}

void Restart() { Exit(EWX_REBOOT, true); }
void Shutdown() { Exit(EWX_POWEROFF, true); }
void SignOut() { Exit(EWX_LOGOFF, false); } // logoff needs no privilege

}

void SetAwake(bool on) { // per-thread, call from the ui thread
    const EXECUTION_STATE flags =
        on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS; // continuous alone clears the request
    if (SetThreadExecutionState(flags) == 0) OutputDebugStringW(L"[sys] SetThreadExecutionState failed\n");
}

void SetEco(bool on) { // win11 ecoqos: e-cores, low clocks; ignored on older windows
    PROCESS_POWER_THROTTLING_STATE ps = {PROCESS_POWER_THROTTLING_CURRENT_VERSION};
    ps.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    ps.StateMask = on ? ps.ControlMask : 0; // 0 = os decides, full speed while open
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &ps, sizeof(ps));
    MEMORY_PRIORITY_INFORMATION mp = {static_cast<ULONG>(on ? MEMORY_PRIORITY_LOW : MEMORY_PRIORITY_NORMAL)}; // our pages get trimmed first
    SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &mp, sizeof(mp));
}

void OpenSnip() { Shell(L"ms-screenclip:"); } // protocol opens snipping overlay

void OpenUri(const wchar_t* uri) {
    if (!uri || !*uri) return;
    Shell(uri);
}

}
