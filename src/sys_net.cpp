#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <winhttp.h>

#include "sys.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace sys {

namespace detail {

constexpr int kResolveMs = 10000;  // dns timeout
constexpr int kConnMs = 10000;
constexpr int kSendMs = 10000;
constexpr int kRecvMs = 15000;  // longer, slow apis stream late

namespace {

void LogHttp(const wchar_t* what, DWORD code) {
    wchar_t buf[128];
    swprintf_s(buf, L"[sys] HTTP %ls failed: %lu\n", what, code);
    OutputDebugStringW(buf);
}

struct HttpH {  // raii winhttp handle
    HINTERNET h = nullptr;
    HttpH() = default;
    explicit HttpH(HINTERNET handle) : h(handle) {}
    HttpH(const HttpH&) = delete;  // no copy, would double close
    HttpH& operator=(const HttpH&) = delete;
    ~HttpH() {
        if (h) WinHttpCloseHandle(h);
    }
};

class LiveReq {  // publishes request handle so another thread can cancel
public:
    LiveReq(HINTERNET req, std::atomic<bool>& stop, std::atomic<void*>& slot)
        : stop_(stop), slot_(slot) {
        slot_.store(req);  // visible to HttpCancel from now on
    }
    LiveReq(const LiveReq&) = delete;
    LiveReq& operator=(const LiveReq&) = delete;
    ~LiveReq() {
        if (void* h = slot_.exchange(nullptr)) WinHttpCloseHandle(static_cast<HINTERNET>(h));  // exchange: only one side closes
    }
    bool Cancelled() const { return stop_.load(); }

private:
    std::atomic<bool>& stop_;
    std::atomic<void*>& slot_;
};

bool ReadBody(HINTERNET req, std::string& body, const LiveReq& active, size_t maxBytes) {
    char chunk[8192];
    for (;;) {
        if (active.Cancelled()) return false;  // check between chunks
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk, sizeof(chunk), &read)) return false;  // fails if handle closed by cancel
        if (read == 0) return true;  // zero bytes = end of body
        if (body.size() + read > maxBytes) return false;  // too big, treat as error
        body.append(chunk, read);
    }
}

}

void HttpCancel(std::atomic<bool>& stop, std::atomic<void*>& slot) {
    stop.store(true);  // set flag first so worker bails after unblock
    if (void* h = slot.exchange(nullptr)) WinHttpCloseHandle(static_cast<HINTERNET>(h));  // cancel closes handle to unblock read
}

bool HttpGet(const wchar_t* url, std::string& body, std::atomic<bool>& stop, std::atomic<void*>& slot, size_t maxBytes) {
    body.clear();
    if (!url || stop.load()) return false;

    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;  // caller buffers, crack copies into them
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra;  // query string lands here, not in path
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        OutputDebugStringW(L"[sys] WinHttpCrackUrl failed\n");
        return false;
    }
    std::wstring object = std::wstring(path) + extra;  // rejoin path + query for request
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HttpH session(WinHttpOpen(L"Astraeus/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,  // honors system/pac proxy
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        OutputDebugStringW(L"[sys] WinHttpOpen failed\n");
        return false;
    }
    WinHttpSetTimeouts(session.h, kResolveMs, kConnMs, kSendMs, kRecvMs);  // ms; defaults are too long
    HttpH connect(WinHttpConnect(session.h, host, uc.nPort, 0));  // no network io yet, just binds host
    if (!connect.h) return false;

    HINTERNET req = WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return false;
    LiveReq active(req, stop, slot);  // owns req; closes it on scope exit
    if (active.Cancelled()) return false;  // cancel may have raced before slot was set

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        active.Cancelled() || !WinHttpReceiveResponse(req, nullptr) || active.Cancelled()) {  // recheck after each blocking call
        if (!active.Cancelled()) LogHttp(L"request", GetLastError());  // cancel errors are expected, skip log
        return false;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,  // status as dword, not text
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        LogHttp(L"status", status);
        return false;
    }
    return ReadBody(req, body, active, maxBytes) && !body.empty();  // empty body counts as failure
}

}

namespace {

constexpr ULONGLONG kInfoMs = 10 * 1000;  // re-pick nic every 10s
constexpr ULONGLONG kWanMs = 10 * 60 * 1000;  // public ip refresh, 10 min
constexpr ULONGLONG kWanRetry = 60 * 1000;  // backoff after failed lookup
constexpr ULONG kProbe = 0x08080808;  // 8.8.8.8, same bytes either endian
constexpr wchar_t kWanUrl[] = L"https://api.ipify.org";  // returns bare ip as text

std::wstring Ipv4Str(const SOCKADDR* sa) {
    if (!sa || sa->sa_family != AF_INET) return {};  // ipv4 only
    const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&in->sin_addr);  // network order bytes
    wchar_t buf[16];  // max "255.255.255.255" + nul
    swprintf_s(buf, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

std::wstring ToWide(const char* s, int len) {
    if (!s || len <= 0) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);  // first pass: size only
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, w.data(), n);  // explicit len, ssid not nul terminated
    return w;
}

bool IsIp(const std::string& s) {  // loose sanity check, rejects html error pages
    if (s.empty() || s.size() > 45) return false;  // 45 = max ipv6 text length
    for (char c : s) {  // digits, dots, colons, hex for v6
        const bool ok = (c >= '0' && c <= '9') || c == '.' || c == ':' || (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

}

struct NetSrc::Impl {
    ULONGLONG infoTick = 0;
    bool haveInfo = false;
    bool online = false;
    NET_LUID luid{};  // active nic id, stable across renames
    IFTYPE ifType = 0;
    std::wstring adapter, lanIp, ssid;
    int signal = -1;  // wifi quality 0-100, -1 = n/a
    ULONG64 prevIn = 0, prevOut = 0;  // last octet counters for rate
    LARGE_INTEGER prevQpc{};
    bool haveCnt = false;  // false until we have a baseline
    LARGE_INTEGER qpcHz{};
    HANDLE wlan = nullptr;  // opened lazily, only on wifi
    bool wlanBad = false;  // wlan service missing, stop retrying
    std::mutex wanMu;  // guards wanIp between worker and ui
    std::wstring wanIp;
    std::thread wanTh;
    std::atomic<bool> wanBusy{false};  // one lookup in flight at a time
    std::atomic<bool> stop{false};
    std::atomic<void*> wanReq{nullptr};  // live request handle for cancel
    std::atomic<ULONGLONG> wanNext{0};  // tick when next lookup allowed

    Impl() {
        QueryPerformanceFrequency(&qpcHz);
    }

    ~Impl() {
        detail::HttpCancel(stop, wanReq);  // unblock worker so join is quick
        if (wanTh.joinable()) wanTh.join();
        if (wlan) WlanCloseHandle(wlan, nullptr);
    }

    void UpdInfo() {
        const ULONGLONG now = GetTickCount64();
        if (haveInfo && now - infoTick < kInfoMs) return;  // throttle, nic lookup is costly
        infoTick = now;
        haveInfo = true;

        const NET_LUID oldLuid = luid;
        PickNic();
        if (oldLuid.Value != luid.Value) haveCnt = false;  // nic changed, old counters meaningless

        ssid.clear();
        signal = -1;
        if (online && ifType == IF_TYPE_IEEE80211) QueryWlan();  // 802.11 = wifi
    }

    void PickNic() {
        online = false;
        adapter.clear();
        lanIp.clear();
        luid.Value = 0;
        ifType = 0;
        SOCKADDR_INET dest{};
        dest.Ipv4.sin_family = AF_INET;
        dest.Ipv4.sin_addr.S_un.S_addr = kProbe;  // probe route to 8.8.8.8 to find active nic
        MIB_IPFORWARD_ROW2 route{};
        SOCKADDR_INET source{};  // filled with local ip used for that route
        const DWORD rc = GetBestRoute2(nullptr, 0, nullptr, &dest, 0, &route, &source);  // no packets sent, table lookup
        if (rc != NO_ERROR) return;  // no default route = offline
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = route.InterfaceLuid;
        if (GetIfEntry2Ex(MibIfEntryNormalWithoutStatistics, &row) != NO_ERROR ||  // skip stats, cheaper
            row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
            return;
        }
        online = true;
        luid = route.InterfaceLuid;
        ifType = row.Type;
        adapter = row.Alias;  // friendly name, e.g. "Wi-Fi"
        lanIp = Ipv4Str(reinterpret_cast<const SOCKADDR*>(&source.Ipv4));
    }

    void QueryWlan() {
        if (wlanBad) return;
        if (!wlan) {
            DWORD wlanVer = 0;
            if (WlanOpenHandle(2, nullptr, &wlanVer, &wlan) != ERROR_SUCCESS) {  // 2 = vista+ client api
                OutputDebugStringW(L"[sys] WlanOpenHandle failed; SSID unavailable\n");
                wlan = nullptr;
                wlanBad = true;
                return;
            }
        }
        GUID ifGuid{};
        if (ConvertInterfaceLuidToGuid(&luid, &ifGuid) != NO_ERROR) return;  // wlan api keys by guid not luid

        DWORD size = 0;
        WLAN_CONNECTION_ATTRIBUTES* attrs = nullptr;
        WLAN_OPCODE_VALUE_TYPE type{};
        if (WlanQueryInterface(wlan, &ifGuid, wlan_intf_opcode_current_connection, nullptr, &size,
                               reinterpret_cast<void**>(&attrs), &type) != ERROR_SUCCESS || !attrs) {  // api allocates attrs
            return;
        }
        if (attrs->isState == wlan_interface_state_connected) {
            const DOT11_SSID& s = attrs->wlanAssociationAttributes.dot11Ssid;
            const ULONG len = s.uSSIDLength <= DOT11_SSID_MAX_LENGTH ? s.uSSIDLength : DOT11_SSID_MAX_LENGTH;  // clamp to 32 bytes
            ssid = ToWide(reinterpret_cast<const char*>(s.ucSSID), static_cast<int>(len));
            signal = static_cast<int>(attrs->wlanAssociationAttributes.wlanSignalQuality);
        }
        WlanFreeMemory(attrs);  // must free with wlan allocator
    }

    void ReadIO(Net& n) {
        if (!online) {
            haveCnt = false;
            return;
        }
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = luid;
        if (GetIfEntry2(&row) != NO_ERROR || row.OperStatus != IfOperStatusUp) {  // full row incl. octet counters
            haveCnt = false;
            online = false;
            haveInfo = false;  // force nic re-pick next sample
            ssid.clear();
            signal = -1;
            return;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        if (haveCnt && qpcHz.QuadPart > 0) {
            const double secs = static_cast<double>(qpc.QuadPart - prevQpc.QuadPart) /  // real elapsed, not timer interval
                                static_cast<double>(qpcHz.QuadPart);
            if (secs > 0) {
                const ULONG64 dIn = row.InOctets >= prevIn ? row.InOctets - prevIn : 0;  // counter reset gives 0 not huge
                const ULONG64 dOut = row.OutOctets >= prevOut ? row.OutOctets - prevOut : 0;
                n.downBps = static_cast<double>(dIn) / secs;  // bytes per sec
                n.upBps = static_cast<double>(dOut) / secs;
            }
        }
        prevIn = row.InOctets;
        prevOut = row.OutOctets;
        prevQpc = qpc;
        haveCnt = true;
    }

    void StartWan(HWND notify, UINT msg) {
        const ULONGLONG now = GetTickCount64();
        if (wanBusy.load() || now < wanNext.load() || stop.load()) return;
        if (wanTh.joinable()) wanTh.join();  // reap finished worker, busy is false so no wait
        wanBusy.store(true);
        wanNext.store(now + kWanRetry);  // assume failure; success pushes it to kWanMs
        try {
            wanTh = std::thread([this, notify, msg] { WanJob(notify, msg); });
        } catch (...) {
            wanBusy.store(false);
            OutputDebugStringW(L"[sys] could not start WAN IP thread\n");
        }
    }

    void WanJob(HWND notify, UINT msg) {
        try {
            std::string body;
            if (detail::HttpGet(kWanUrl, body, stop, wanReq)) {
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {  // trim trailing whitespace
                    body.pop_back();
                }
                if (IsIp(body)) {
                    std::wstring ip(body.begin(), body.end());
                    {
                        std::lock_guard<std::mutex> lock(wanMu);
                        wanIp = std::move(ip);
                    }
                    wanNext.store(GetTickCount64() + kWanMs);
                    if (!stop.load() && notify && IsWindow(notify)) PostMessageW(notify, msg, 0, 0);  // wake ui thread to repaint
                }
            }
        } catch (...) {
            OutputDebugStringW(L"[sys] WAN IP worker failed\n");
        }
        wanBusy.store(false);
    }
};

NetSrc::NetSrc() {
    try {
        impl = std::make_unique<Impl>();
    } catch (...) {
        OutputDebugStringW(L"[sys] NetSampler init failed\n");
    }
}

NetSrc::~NetSrc() = default;

Net NetSrc::Sample() {
    Net n;
    try {
        if (!impl) return n;
        impl->UpdInfo();
        impl->ReadIO(n);
        n.online = impl->online;
        n.adapter = impl->adapter;
        n.lanIp = impl->lanIp;
        n.ssid = impl->ssid;
        n.signal = impl->signal;
        std::lock_guard<std::mutex> lock(impl->wanMu);  // worker may be writing wanIp
        n.wanIp = impl->wanIp;
    } catch (...) {
        OutputDebugStringW(L"[sys] NetSampler::Sample failed\n");
    }
    return n;
}

void NetSrc::FetchWan(HWND notify, UINT msg) {
    try {
        if (!impl) return;
        impl->StartWan(notify, msg);
    } catch (...) {
        OutputDebugStringW(L"[sys] RequestWanIp failed\n");
    }
}

}
