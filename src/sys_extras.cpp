#include "extras.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wuapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")

using Microsoft::WRL::ComPtr;

namespace sys::extras {
namespace {

constexpr DWORD kPingMs = 1500, kEveryMs = 60000, kPackagesMs = 60000; // a host's answer, the gap between rounds, the package tool's whole run
constexpr size_t kTitleMax = 120;

std::string Capture(const std::wstring& cmd, DWORD waitMs) { // a hidden command's whole output
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES, si.hStdOutput = wr, si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    bool ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
    CloseHandle(wr);
    std::string out;
    if (ok) {
        CloseHandle(pi.hThread);
        char buf[8192];
        DWORD got = 0;
        while (out.size() < (1 << 20) && ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
        if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}

int PackageUpdates() { // the package tool's count: "N upgrades available.", or a table with none
    std::string out = Capture(L"winget upgrade --accept-source-agreements --disable-interactivity", kPackagesMs);
    if (out.empty()) return -1; // not installed
    size_t at = out.find("upgrades available");
    if (at == std::string::npos) at = out.find("upgrade available");
    if (at == std::string::npos) return 0;
    size_t end = at;
    while (end > 0 && out[end - 1] == ' ') --end;
    size_t start = end;
    while (start > 0 && isdigit((unsigned char)out[start - 1])) --start;
    return start < end ? atoi(out.substr(start, end - start).c_str()) : 0;
}

int SystemUpdates() { // windows update's own count of what's waiting, hidden ones aside
    ComPtr<IUpdateSession> session;
    ComPtr<IUpdateSearcher> searcher;
    ComPtr<ISearchResult> result;
    ComPtr<IUpdateCollection> found;
    BSTR query = SysAllocString(L"IsInstalled=0 and IsHidden=0");
    LONG n = -1;
    if (SUCCEEDED(CoCreateInstance(__uuidof(UpdateSession), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&session))) && SUCCEEDED(session->CreateUpdateSearcher(&searcher))) {
        searcher->put_Online(VARIANT_FALSE); // what windows already knows: no download of the catalogue just for a count
        if (SUCCEEDED(searcher->Search(query, &result)) && SUCCEEDED(result->get_Updates(&found))) found->get_Count(&n);
    }
    SysFreeString(query);
    return (int)n;
}

bool IsWeb(const std::wstring& h) { return _wcsnicmp(h.c_str(), L"http://", 7) == 0 || _wcsnicmp(h.c_str(), L"https://", 8) == 0; }

HostHealth Ask(const std::wstring& host) {
    HostHealth r{host, false, true, -1};
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
    if (IsWeb(host)) { // a web address: up when it answers below 500
        URL_COMPONENTS u{sizeof(u)};
        wchar_t name[256] = {}, path[1024] = {};
        u.lpszHostName = name, u.dwHostNameLength = 256, u.lpszUrlPath = path, u.dwUrlPathLength = 1024;
        if (!WinHttpCrackUrl(host.c_str(), 0, 0, &u)) return r;
        HINTERNET s = WinHttpOpen(L"Astraeus", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) return r;
        WinHttpSetTimeouts(s, 3000, 3000, 3000, 5000);
        HINTERNET c = WinHttpConnect(s, name, u.nPort, 0);
        HINTERNET q = c ? WinHttpOpenRequest(c, L"HEAD", path[0] ? path : L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
        DWORD code = 0, len = sizeof(code);
        if (q && WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(q, nullptr) &&
            WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr))
            r.up = code > 0 && code < 500, r.ms = ms();
        if (q) WinHttpCloseHandle(q);
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return r;
    }
    ADDRINFOW hints{}, *res = nullptr; // a name or an address: one ping
    hints.ai_family = AF_INET;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &res) || !res) return r;
    IPAddr ip = ((sockaddr_in*)res->ai_addr)->sin_addr.S_un.S_addr;
    FreeAddrInfoW(res);
    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) return r;
    char data[8] = "astraeu";
    char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 16];
    if (IcmpSendEcho(icmp, ip, data, sizeof(data), nullptr, reply, sizeof(reply), kPingMs) > 0) {
        auto* e = (ICMP_ECHO_REPLY*)reply;
        r.up = e->Status == IP_SUCCESS, r.ms = (int)e->RoundTripTime;
    }
    IcmpCloseHandle(icmp);
    return r;
}

}

std::wstring Language() {
    HWND fg = GetForegroundWindow();
    HKL hkl = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {}, iso[16] = {};
    if (!LCIDToLocaleName(MAKELCID(LOWORD((UINT_PTR)hkl), SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0) || !GetLocaleInfoEx(locale, LOCALE_SISO639LANGNAME, iso, 16)) return L"";
    std::wstring s = iso;
    for (auto& c : s) c = (wchar_t)std::towupper(c);
    return s;
}

void NextLanguage() {
    HWND fg = GetForegroundWindow();
    if (fg) PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)HKL_NEXT); // the app in front asks windows for its next keyboard
}

std::wstring ActiveTitle() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId())) return L"";
    wchar_t cls[16] = {};
    GetClassNameW(fg, cls, 16);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return L""; // the desktop itself: no window to name, and its title is "program manager"
    wchar_t buf[512];
    int n = GetWindowTextW(fg, buf, 512);
    std::wstring s(buf, (std::max)(0, n));
    if (s.size() > kTitleMax) s = s.substr(0, kTitleMax - 1) + L"\u2026";
    return s;
}

bool SystemDrive(float& used, double& freeGb) {
    wchar_t win[MAX_PATH];
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return false;
    std::wstring root = std::wstring(win).substr(0, 3); // "C:\"
    ULARGE_INTEGER avail{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &free) || total.QuadPart == 0) return false;
    used = 1 - (float)((double)free.QuadPart / (double)total.QuadPart);
    freeGb = (double)avail.QuadPart / (1024.0 * 1024 * 1024);
    return true;
}

BinInfo Bin() {
    SHQUERYRBINFO q{sizeof(q)};
    BinInfo b;
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &q))) b.bytes = q.i64Size, b.items = q.i64NumItems;
    return b;
}

void EmptyBin(HWND owner) { SHEmptyRecycleBinW(owner, nullptr, 0); } // no flags: windows' own "are you sure"

struct Updates::Impl {
    mutable std::mutex mu;
    UpdateCount count;
};

Updates::Updates() : impl(std::make_shared<Impl>()) {}
Updates::~Updates() = default;

void Updates::Check(HWND notify, UINT msg) {
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->count.busy) return;
        impl->count.busy = true;
    }
    auto self = impl;
    std::thread([self, notify, msg] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int system = SystemUpdates();
        int apps = PackageUpdates();
        if (SUCCEEDED(co)) CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(self->mu);
            self->count = {system, apps, false};
        }
        PostMessageW(notify, msg, 0, 0);
    }).detach(); // holds only its own copy of the state: safe to outlive the panel
}

UpdateCount Updates::Get() const {
    std::lock_guard<std::mutex> lock(impl->mu);
    return impl->count;
}

struct Pinger::Impl {
    HWND hwnd = nullptr;
    UINT msg = 0;
    HANDLE stop = nullptr, wake = nullptr;
    std::thread worker;
    mutable std::mutex mu;
    std::vector<std::wstring> hosts;
    std::vector<HostHealth> results;
};

Pinger::Pinger() = default;
Pinger::~Pinger() { Stop(); }

void Pinger::Start(HWND notify, UINT msg, const std::vector<std::wstring>& hosts) {
    Stop();
    if (hosts.empty()) return;
    impl = std::make_shared<Impl>();
    impl->hwnd = notify, impl->msg = msg, impl->hosts = hosts;
    for (const auto& h : hosts) impl->results.push_back({h});
    impl->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto self = impl;
    impl->worker = std::thread([self] {
        for (;;) {
            for (size_t i = 0; i < self->hosts.size(); ++i) {
                if (WaitForSingleObject(self->stop, 0) == WAIT_OBJECT_0) return;
                HostHealth h = Ask(self->hosts[i]);
                {
                    std::lock_guard<std::mutex> lock(self->mu);
                    self->results[i] = h;
                }
                PostMessageW(self->hwnd, self->msg, 0, 0);
            }
            HANDLE waits[2] = {self->stop, self->wake};
            if (WaitForMultipleObjects(2, waits, FALSE, kEveryMs) == WAIT_OBJECT_0) return;
        }
    });
}

void Pinger::Stop() {
    if (!impl) return;
    SetEvent(impl->stop);
    if (impl->worker.joinable()) impl->worker.join(); // at most one host's timeout away
    CloseHandle(impl->stop);
    CloseHandle(impl->wake);
    impl.reset();
}

bool Pinger::On() const { return impl != nullptr; }

void Pinger::Now() {
    if (impl) SetEvent(impl->wake);
}

std::vector<HostHealth> Pinger::Results() const {
    if (!impl) return {};
    std::lock_guard<std::mutex> lock(impl->mu);
    return impl->results;
}

}
