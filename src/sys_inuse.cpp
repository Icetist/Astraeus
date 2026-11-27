#include "sys_inuse.h"

#include <algorithm>

#pragma comment(lib, "version.lib")

namespace sys::inuse {

namespace {

constexpr wchar_t kStore[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
constexpr const wchar_t* kKinds[] = {L"microphone", L"webcam"};
constexpr DWORD kSettleMs = 150; // an app writes its start and stop a moment apart: read once they've settled

bool Using(HKEY app) { // started and not yet stopped
    ULONGLONG start = 0, stop = 0;
    DWORD size = sizeof(start);
    if (RegGetValueW(app, nullptr, L"LastUsedTimeStart", RRF_RT_QWORD, nullptr, &start, &size) != ERROR_SUCCESS) return false;
    size = sizeof(stop);
    if (RegGetValueW(app, nullptr, L"LastUsedTimeStop", RRF_RT_QWORD, nullptr, &stop, &size) != ERROR_SUCCESS) return false;
    return start != 0 && stop == 0;
}

std::wstring FromPath(const std::wstring& key) { // "C:#Program Files#Discord#Discord.exe": the program's own name for itself, else its file's
    std::wstring path = key;
    std::replace(path.begin(), path.end(), L'#', L'\\');
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size) {
        std::vector<BYTE> info(size);
        struct Lang {
            WORD lang, page;
        }* langs = nullptr;
        UINT n = 0;
        wchar_t* desc = nullptr;
        if (GetFileVersionInfoW(path.c_str(), 0, size, info.data()) && VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &n) && n >= sizeof(Lang)) {
            wchar_t q[64];
            swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].page);
            if (VerQueryValueW(info.data(), q, (void**)&desc, &n) && n > 1 && desc && *desc) return desc;
        }
    }
    size_t slash = path.find_last_of(L'\\');
    std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (file.size() > 4 && _wcsicmp(file.c_str() + file.size() - 4, L".exe") == 0) file.resize(file.size() - 4);
    return file;
}

std::wstring FromPackage(const std::wstring& family) { // "Microsoft.WindowsCamera_8wekyb3d8bbwe": the last word before the publisher
    std::wstring s = family.substr(0, family.find(L'_'));
    size_t dot = s.find_last_of(L'.');
    return dot == std::wstring::npos ? s : s.substr(dot + 1);
}

std::wstring SelfKey() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe, n);
    std::replace(s.begin(), s.end(), L'\\', L'#');
    return s;
}

template <typename F>
void EachSub(HKEY parent, F f) {
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = (DWORD)std::size(name);
        if (RegEnumKeyExW(parent, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(parent, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        f(std::wstring(name, len), sub);
        RegCloseKey(sub);
    }
}

}

void Watch::Start(HWND notify, UINT msg) {
    if (run.joinable()) return;
    hwnd = notify, message = msg;
    quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!quit) return;
    run = std::thread([this] { Run(); });
}

void Watch::Stop() {
    if (!run.joinable()) return;
    SetEvent(quit);
    run.join();
    CloseHandle(quit);
    quit = nullptr;
    std::lock_guard<std::mutex> lock(mu);
    users.clear();
}

std::vector<User> Watch::Now() const {
    std::lock_guard<std::mutex> lock(mu);
    return users;
}

void Watch::Scan() {
    static const std::wstring self = SelfKey();
    std::vector<User> found;
    auto mark = [&](const std::wstring& name, int kind) {
        auto it = std::find_if(found.begin(), found.end(), [&](const User& u) { return u.name == name; });
        if (it == found.end()) found.push_back({name}), it = found.end() - 1;
        (kind == 0 ? it->mic : it->cam) = true;
    };
    for (int kind = 0; kind < 2; ++kind) {
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(kStore) + kKinds[kind]).c_str(), 0, KEY_READ, &root) != ERROR_SUCCESS) continue;
        EachSub(root, [&](const std::wstring& name, HKEY app) {
            if (name == L"NonPackaged") {
                EachSub(app, [&](const std::wstring& path, HKEY prog) {
                    if (_wcsicmp(path.c_str(), self.c_str()) != 0 && Using(prog)) mark(FromPath(path), kind); // astraeus' own listening never counts
                });
            } else if (Using(app)) {
                mark(FromPackage(name), kind);
            }
        });
        RegCloseKey(root);
    }
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mu);
        changed = found.size() != users.size() || !std::equal(found.begin(), found.end(), users.begin(), [](const User& a, const User& b) { return a.name == b.name && a.mic == b.mic && a.cam == b.cam; });
        if (changed) users = std::move(found);
    }
    if (changed && hwnd) PostMessageW(hwnd, message, 0, 0);
}

void Watch::Run() {
    HKEY keys[2] = {};
    HANDLE wake[3] = {quit, CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    constexpr DWORD kWhat = REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME;
    for (int kind = 0; kind < 2; ++kind) {
        RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(kStore) + kKinds[kind]).c_str(), 0, KEY_NOTIFY | KEY_READ, &keys[kind]);
        if (keys[kind] && wake[kind + 1]) RegNotifyChangeKeyValue(keys[kind], TRUE, kWhat, wake[kind + 1], TRUE);
    }
    Scan(); // who has them already
    for (;;) {
        DWORD got = WaitForMultipleObjects(3, wake, FALSE, INFINITE);
        if (got == WAIT_OBJECT_0 || got == WAIT_FAILED) break;
        int kind = (int)(got - WAIT_OBJECT_0) - 1;
        if (kind >= 0 && kind < 2 && keys[kind]) RegNotifyChangeKeyValue(keys[kind], TRUE, kWhat, wake[kind + 1], TRUE); // armed again before reading, so nothing slips between
        if (WaitForSingleObject(quit, kSettleMs) == WAIT_OBJECT_0) break;
        Scan();
    }
    for (int kind = 0; kind < 2; ++kind) {
        if (keys[kind]) RegCloseKey(keys[kind]);
        if (wake[kind + 1]) CloseHandle(wake[kind + 1]);
    }
}

}
