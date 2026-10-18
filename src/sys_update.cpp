#include "sys.h"

#include <bcrypt.h>

#include <cctype>
#include <cstring>
#include <thread>

#include "version.h"

#pragma comment(lib, "bcrypt.lib")

namespace sys {
namespace {

constexpr const wchar_t* kLatest[] = {L"https://api.github.com/repos/Icetist/Astraeus-Island/releases/latest", // public api, no token needed
                                      L"https://api.github.com/repos/Icetist/Astraeus/releases/latest"}; // the name before the rename
constexpr char kAsset[] = "/Astraeus.exe"; // release asset the workflow uploads
constexpr char kHashAsset[] = "/Astraeus.exe.sha256"; // sha-256 of that exe, uploaded by the same workflow
constexpr size_t kMaxHashFile = 1024; // 64 hex chars plus maybe a filename
constexpr size_t kHexLen = 64; // sha-256 as hex
constexpr const char* kTrusted[] = {"https://github.com/Icetist/Astraeus-Island/releases/download/", // assets must come from this repo's releases
                                    "https://github.com/Icetist/Astraeus/releases/download/"}; // under either of its names
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

std::string AssetUrl(const std::string& body, const char* name) { // download url of the asset whose url ends with name
    const size_t n = std::strlen(name);
    for (size_t p = body.find("\"browser_download_url\""); p != std::string::npos; p = body.find("\"browser_download_url\"", p + 1)) {
        std::string u = JsonStr(body, p);
        if (u.size() > n && u.compare(u.size() - n, n, name) == 0) return u;
    }
    return {};
}

std::string Sha256Hex(const std::string& data) { // lowercase hex, empty on failure
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32];
    std::string hex;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return hex;
    if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(h, digest, sizeof(digest), 0))) {
        static const char kHex[] = "0123456789abcdef";
        for (unsigned char b : digest) {
            hex += kHex[b >> 4];
            hex += kHex[b & 15];
        }
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

std::string ParseHash(const std::string& file) { // first run of 64 hex chars, lowercased; empty if malformed
    std::string hex;
    for (char ch : file) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (hex.size() > kHexLen) return {}; // too long, not a sha-256
        } else if (!hex.empty()) {
            break; // hash ended, e.g. "<hash>  Astraeus.exe"
        }
    }
    return hex.size() == kHexLen ? hex : std::string();
}

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
    std::atomic<bool> manual{false}; // someone pressed check: answer even if there's nothing new, and don't make them wait on retries
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
        const bool quick = manual.load();
        for (int i = 0; i < (quick ? 2 : kTries) && !stop.load(); ++i) {
            if (i) Wait(quick ? 1500 : kRetryMs);
            if (detail::HttpGet(url, out, stop, req, max)) return true;
        }
        return false;
    }

    Updater::Result Fetch(HWND notify, UINT msg) {
        std::string body;
        if (!Get(kLatest[0], body, 256 * 1024) && !Get(kLatest[1], body, 256 * 1024)) return Updater::kFailed; // still offline, next scheduled check retries
        size_t k = body.find("\"tag_name\"");
        std::string t = k == std::string::npos ? "" : JsonStr(body, k);
        if (t.empty()) return Updater::kFailed;
        if (!Newer(t)) return Updater::kCurrent;
        if (!manual.load() && Widen(t) == Done()) return Updater::kFailed; // this tag was already tried on its own; a press tries again
        const std::string url = AssetUrl(body, kAsset), hashUrl = AssetUrl(body, kHashAsset);
        if (url.empty() || hashUrl.empty()) return Updater::kCurrent; // still building, or a release without a published hash: never install unverified
        auto trusted = [](const std::string& u) { return u.rfind(kTrusted[0], 0) == 0 || u.rfind(kTrusted[1], 0) == 0; };
        if (!trusted(url) || !trusted(hashUrl)) return Updater::kFailed; // https, this repo only
        tag = Widen(t); // the menu can say what it's fetching
        if (manual.load() && IsWindow(notify)) PostMessageW(notify, msg, Updater::kDownloading, 0);
        std::string sum;
        if (!Get(Widen(hashUrl).c_str(), sum, kMaxHashFile)) return Updater::kFailed;
        const std::string want = ParseHash(sum);
        if (want.empty()) return Updater::kFailed;
        std::string exe;
        if (!Get(Widen(url).c_str(), exe, kMaxExe) || exe.size() < kMinExe || exe[0] != 'M' || exe[1] != 'Z')
            return Updater::kFailed; // must look like a windows exe
        if (Sha256Hex(exe) != want) { // corrupted or tampered download: not marked done, so the next check retries
            OutputDebugStringW(L"[sys] update rejected: sha-256 mismatch\n");
            return Updater::kFailed;
        }
        MarkDone(Widen(t)); // once only, even if the swap fails
        return Swap(exe) ? Updater::kInstalled : Updater::kFailed;
    }
};

Updater::Updater() = default;
Updater::~Updater() = default;

void Updater::Check(HWND notify, UINT msg, bool manual) {
    try {
        if (!impl) impl = std::make_unique<Impl>();
        if (manual) impl->manual.store(true); // a check already underway answers this press too
        if (impl->busy.exchange(true)) return; // one check at a time
        if (impl->th.joinable()) impl->th.join();
        impl->th = std::thread([p = impl.get(), notify, msg] {
            Result r = kFailed;
            try {
                r = p->Fetch(notify, msg);
            } catch (...) {
                OutputDebugStringW(L"[sys] update check failed\n");
            }
            bool asked = p->manual.exchange(false);
            p->busy.store(false);
            if ((r == kInstalled || asked) && !p->stop.load() && IsWindow(notify)) PostMessageW(notify, msg, r, 0); // ui restarts when idle
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
