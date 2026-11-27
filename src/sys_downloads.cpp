#include "sys_downloads.h"

#include <shlobj.h>

#include <algorithm>

namespace sys::downloads {

namespace {

constexpr DWORD kMeasureMs = 500; // how often a growing file is looked at
constexpr ULONGLONG kStillMs = 15000; // not grown this long: paused or stuck, not shown
constexpr ULONGLONG kGiveUpMs = 60000; // not grown this long: forgotten, so a leftover never keeps the thread looking
constexpr ULONGLONG kFreshMs = 60000; // at start, only partial files written this recently are downloads still going
constexpr double kRateMix = 0.3; // each new reading's share of the speed: steady, but quick to follow
constexpr size_t kDoneMost = 8;
constexpr DWORD kBuf = 16 * 1024;

const wchar_t* const kPartial[] = {L".crdownload", L".part", L".partial"};

size_t PartialExt(const std::wstring& f) { // the length of its in-progress ending, 0 if it's a whole file
    for (const wchar_t* e : kPartial) {
        size_t n = wcslen(e);
        if (f.size() > n && _wcsicmp(f.c_str() + f.size() - n, e) == 0) return n;
    }
    return 0;
}

ULONGLONG SizeOf(const std::wstring& path, ULONGLONG* writtenMs = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    if (writtenMs) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULONGLONG a = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime, b = ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
        *writtenMs = a > b ? (a - b) / 10000 : 0;
    }
    return ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

}

void Watch::Start(HWND notify, UINT msg) {
    if (run.joinable()) return;
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) return;
    dir = path;
    CoTaskMemFree(path);
    hwnd = notify, message = msg;
    quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (quit) run = std::thread([this] { Run(); });
}

void Watch::Stop() {
    if (!run.joinable()) return;
    SetEvent(quit);
    run.join();
    CloseHandle(quit);
    quit = nullptr;
    std::lock_guard<std::mutex> lock(mu);
    parts.clear(), done.clear();
}

std::vector<Going> Watch::Now() const {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<Going> out;
    ULONGLONG now = GetTickCount64();
    for (const Part& p : parts)
        if (now - p.lastAt < kStillMs) out.push_back({p.name, p.bytes, p.perSec});
    return out;
}

bool Watch::TakeDone(Done& out) {
    std::lock_guard<std::mutex> lock(mu);
    if (done.empty()) return false;
    out = std::move(done.front());
    done.erase(done.begin());
    return true;
}

void Watch::Seen(const std::wstring& file, bool gone) { // under mu
    auto it = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return _wcsicmp(p.file.c_str(), file.c_str()) == 0; });
    if (gone) {
        if (it != parts.end()) parts.erase(it);
        return;
    }
    size_t ext = PartialExt(file);
    if (!ext || it != parts.end()) return;
    Part p;
    p.file = file, p.name = file.substr(0, file.size() - ext);
    p.lastAt = GetTickCount64();
    parts.push_back(std::move(p));
}

void Watch::Measure() {
    bool any;
    {
        std::lock_guard<std::mutex> lock(mu);
        ULONGLONG now = GetTickCount64();
        for (Part& p : parts) {
            ULONGLONG b = SizeOf(dir + L"\\" + p.file);
            if (b > p.bytes) {
                double secs = (double)(now - p.lastAt) / 1000.0, rate = secs > 0 ? (double)(b - p.bytes) / secs : 0;
                p.perSec = p.perSec <= 0 ? rate : p.perSec + (rate - p.perSec) * kRateMix;
                p.bytes = b, p.lastAt = now;
            }
        }
        std::erase_if(parts, [&](const Part& p) { return now - p.lastAt > kGiveUpMs; });
        any = !parts.empty();
    }
    if (any && hwnd) PostMessageW(hwnd, message, 0, 0);
}

void Watch::Run() {
    HANDLE folder = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (folder == INVALID_HANDLE_VALUE) return;
    { // already going when astraeus started
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> lock(mu);
            do {
                ULONGLONG ago = kFreshMs;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && PartialExt(fd.cFileName)) SizeOf(dir + L"\\" + fd.cFileName, &ago);
                if (ago < kFreshMs) Seen(fd.cFileName, false);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    std::vector<BYTE> buf(kBuf);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::wstring renamedFrom;
    while (ov.hEvent) {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(folder, buf.data(), kBuf, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &ov, nullptr)) break;
        HANDLE wake[2] = {quit, ov.hEvent};
        bool stop = false, got = false;
        while (!got && !stop) {
            bool going;
            {
                std::lock_guard<std::mutex> lock(mu);
                going = !parts.empty();
            }
            DWORD w = WaitForMultipleObjects(2, wake, FALSE, going ? kMeasureMs : INFINITE); // asleep unless something's growing
            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) stop = true;
            else if (w == WAIT_TIMEOUT) Measure();
            else got = true;
        }
        if (stop) {
            CancelIoEx(folder, &ov);
            WaitForSingleObject(ov.hEvent, INFINITE);
            break;
        }
        DWORD n = 0;
        if (!GetOverlappedResult(folder, &ov, &n, FALSE)) break;
        bool finished = false;
        if (n) {
            std::lock_guard<std::mutex> lock(mu);
            for (auto* e = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data());; e = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(e) + e->NextEntryOffset)) {
                std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
                switch (e->Action) {
                case FILE_ACTION_ADDED: Seen(name, false); break;
                case FILE_ACTION_REMOVED: Seen(name, true); break; // cancelled
                case FILE_ACTION_RENAMED_OLD_NAME: renamedFrom = name; break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    if (PartialExt(renamedFrom) && !PartialExt(name)) { // whole now
                        Seen(renamedFrom, true);
                        if (done.size() < kDoneMost) done.push_back({name, dir + L"\\" + name});
                        finished = true;
                    } else {
                        Seen(renamedFrom, true), Seen(name, false); // chrome's "Unconfirmed 1234" taking its real name
                    }
                    renamedFrom.clear();
                    break;
                default: break;
                }
                if (!e->NextEntryOffset) break;
            }
        }
        if (finished && hwnd) PostMessageW(hwnd, message, 1, 0);
        else Measure();
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(folder);
}

}
