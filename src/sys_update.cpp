#include "sys.h"

#include <cctype>
#include <cstring>
#include <thread>

#include "version.h"

namespace sys {
namespace {

constexpr wchar_t kLatest[] = L"https://api.github.com/repos/Icetist/Astraeus/releases/latest"; // public api, no token needed
constexpr char kAsset[] = "/Astraeus.exe"; // release asset the workflow uploads
constexpr size_t kMaxExe = 32u << 20; // refuse anything absurdly large
constexpr size_t kMinExe = 64u << 10; // or suspiciously small
constexpr wchar_t kKey[] = L"Software\\Astraeus";
constexpr wchar_t kDoneVal[] = L"UpdatedTo"; // last tag we installed, stops retry loops
constexpr int kTries = 4; // network blips are common right after wake/boot
constexpr DWORD kRetryMs = 30000;

std::string JsonStr(const std::string& s, size_t key) { // string value after "key":
    size_t colon = s.find(':', key);
    size_t q = colon == std::string::npos ? colon : s.find('"', colon + 1);
    if (q == std::string::npos) return {};
    size_t e = s.find('"', q + 1);
    return e == std::string::npos ? std::string() : s.substr(q + 1, e - q - 1);
}

bool Newer(const std::string& tag) { // "v2.3.1" vs compiled-in version
    int v[3] = {};
    int i = 0;
    for (char ch : tag) {
        if (std::isdigit(static_cast<unsigned char>(ch))) v[i] = v[i] * 10 + (ch - '0');
        else if (ch == '.' && ++i == 3) break; // ignore anything past patch
    }
    const int cur[3] = {APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH};
    for (int k = 0; k < 3; ++k)
        if (v[k] != cur[k]) return v[k] > cur[k];
    return false;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); } // tags and urls are ascii

std::wstring Done() {
    wchar_t v[64] = {};
    DWORD size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, kDoneVal, RRF_RT_REG_SZ, nullptr, v, &size) != ERROR_SUCCESS) return L"";
    return v;
}

void MarkDone(const std::wstring& tag) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, kDoneVal, REG_SZ, tag.c_str(), (DWORD)((tag.size() + 1) * sizeof(wchar_t)));
}

std::wstring SelfDir(std::wstring* exe = nullptr) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    if (exe) *exe = path;
    return path.substr(0, path.find_last_of(L'\\'));
}

bool Swap(const std::string& data) { // write new exe beside us, rename ourselves aside, move new in
    std::wstring exe, dir = SelfDir(&exe);
    std::wstring neu = dir + L"\\Astraeus.new", old = dir + L"\\Astraeus.old";
    HANDLE f = CreateFileW(neu.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false; // read-only folder, e.g. program files without admin
    DWORD put = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(neu.c_str());
        return false;
    }
    DeleteFileW(old.c_str()); // leftover from a previous update
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { // a running exe can be renamed, not overwritten
        DeleteFileW(neu.c_str());
        return false;
    }
    if (!MoveFileExW(neu.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), 0); // roll back so we still have an exe
        return false;
    }
    return true;
}

}

struct Updater::Impl {
    std::thread th;
    std::atomic<bool> busy{false}, stop{false};
    std::atomic<void*> req{nullptr}; // live http handle, closed to cancel
    std::wstring tag; // installed version, read by ui after the post

    ~Impl() {
        detail::HttpCancel(stop, req);
        if (th.joinable()) th.join();
    }

    void Wait(DWORD ms) { // sleep that wakes early on shutdown
        for (DWORD t = 0; t < ms && !stop.load(); t += 100) ::Sleep(100);
    }

    bool Get(const wchar_t* url, std::string& out, size_t max) { // retried get
        for (int i = 0; i < kTries && !stop.load(); ++i) {
            if (i) Wait(kRetryMs);
            if (detail::HttpGet(url, out, stop, req, max)) return true;
        }
        return false;
    }

    bool Fetch() {
        std::string body;
        if (!Get(kLatest, body, 256 * 1024)) return false; // still offline, next scheduled check retries
        size_t k = body.find("\"tag_name\"");
        std::string t = k == std::string::npos ? "" : JsonStr(body, k);
        if (t.empty() || !Newer(t) || Widen(t) == Done()) return false; // up to date, or this tag already tried
        std::string url;
        const size_t n = std::strlen(kAsset);
        for (size_t p = body.find("\"browser_download_url\""); p != std::string::npos; p = body.find("\"browser_download_url\"", p + 1)) {
            std::string u = JsonStr(body, p);
            if (u.size() > n && u.compare(u.size() - n, n, kAsset) == 0) {
                url = u;
                break;
            }
        }
        if (url.empty()) return false; // release without our asset, e.g. still building
        std::string exe;
        if (!Get(Widen(url).c_str(), exe, kMaxExe) || exe.size() < kMinExe || exe[0] != 'M' || exe[1] != 'Z')
            return false; // must look like a windows exe
        MarkDone(Widen(t)); // once only, even if the swap fails
        if (!Swap(exe)) return false;
        tag = Widen(t);
        return true;
    }
};

Updater::Updater() = default;
Updater::~Updater() = default;

void Updater::Check(HWND notify, UINT msg) {
    try {
        if (!impl) impl = std::make_unique<Impl>();
        if (impl->busy.exchange(true)) return; // one check at a time
        if (impl->th.joinable()) impl->th.join();
        impl->th = std::thread([p = impl.get(), notify, msg] {
            bool ok = false;
            try {
                ok = p->Fetch();
            } catch (...) {
                OutputDebugStringW(L"[sys] update check failed\n");
            }
            p->busy.store(false);
            if (ok && !p->stop.load() && IsWindow(notify)) PostMessageW(notify, msg, 0, 0); // ui restarts when idle
        });
    } catch (...) {
        if (impl) impl->busy.store(false);
    }
}

std::wstring Updater::Applied() const { return impl ? impl->tag : L""; }

bool FinishUpdate() { // run at launch: remove the renamed old exe, true if we were just updated
    std::wstring old = SelfDir() + L"\\Astraeus.old";
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    for (int i = 0; i < 10 && !DeleteFileW(old.c_str()); ++i) ::Sleep(200); // old process may still be exiting
    return true;
}

}
