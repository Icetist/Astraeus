#include "workspaces.h"

#include <objbase.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace sys {
namespace {

constexpr wchar_t kTilingExe[] = L"komorebi.exe", kTilingCli[] = L"komorebic.exe"; // the pipe-based manager and its command line
constexpr wchar_t kTilingWsExe[] = L"glazewm.exe"; // the socket-based one
constexpr wchar_t kTilingCliExe[] = L"leopardwm.exe", kTilingCliTool[] = L"leopardwm-cli.exe"; // the one read through its command line
constexpr wchar_t kFancyExe[] = L"fancywm.exe", kAmethystExe[] = L"amethystwindows.exe"; // these two tile on windows' own virtual desktops
constexpr INTERNET_PORT kTilingWsPort = 6123;
constexpr wchar_t kDesksKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
constexpr DWORD kLookMs = 3000; // how often the desktops source checks whether a tiling manager started
constexpr DWORD kConnectMs = 5000, kCaptureMs = 5000, kRetryMs = 2000;
constexpr size_t kMaxJson = 8 << 20; // a state larger than this is not a state
constexpr int kMaxSpaces = 32, kMaxDepth = 64;

// ---- a small json reader: enough for the managers' state, nothing more ----
struct J {
    enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J& operator[](const char* key) const {
        static const J none;
        if (type == kObj)
            for (const auto& kv : o)
                if (kv.first == key) return kv.second;
        return none;
    }
    const J& At(size_t i) const {
        static const J none;
        return type == kArr && i < a.size() ? a[i] : none;
    }
    int Int(int def = 0) const { return type == kNum ? (int)n : def; }
};

class Reader {
public:
    Reader(const char* begin, const char* end) : p(begin), e(end) {}
    bool Parse(J& out, int depth = 0) {
        if (depth > kMaxDepth) return false; // nothing real nests this deep
        Skip();
        if (p >= e) return false;
        char c = *p;
        if (c == '{') return Obj(out, depth);
        if (c == '[') return Arr(out, depth);
        if (c == '"') return out.type = J::kStr, Str(out.s);
        if (c == 't' || c == 'f') return Word(out, c == 't');
        if (c == 'n') {
            if (e - p < 4 || strncmp(p, "null", 4)) return false;
            p += 4, out.type = J::kNull;
            return true;
        }
        return Num(out);
    }

private:
    const char *p, *e;
    void Skip() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    bool Word(J& out, bool v) {
        size_t len = v ? 4 : 5;
        if ((size_t)(e - p) < len || strncmp(p, v ? "true" : "false", len)) return false;
        p += len, out.type = J::kBool, out.b = v;
        return true;
    }
    bool Num(J& out) {
        std::string tmp(p, (std::min)((size_t)(e - p), (size_t)64));
        char* end = nullptr;
        out.n = strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        p += end - tmp.c_str(), out.type = J::kNum;
        return true;
    }
    static void Utf8(std::string& s, unsigned cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) s += (char)(0xC0 | (cp >> 6)), s += (char)(0x80 | (cp & 0x3F));
        else s += (char)(0xE0 | (cp >> 12)), s += (char)(0x80 | ((cp >> 6) & 0x3F)), s += (char)(0x80 | (cp & 0x3F));
    }
    bool Str(std::string& s) {
        ++p; // the opening quote
        while (p < e && *p != '"') {
            if (*p != '\\') {
                s += *p++;
                continue;
            }
            if (++p >= e) return false;
            char c = *p++;
            switch (c) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u':
                if (e - p < 4) return false;
                Utf8(s, (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16)); // a surrogate pair comes out as two marks: names rarely carry them
                p += 4;
                break;
            default: s += c; break; // \" \\ and \/
            }
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool Arr(J& out, int depth) {
        out.type = J::kArr;
        ++p;
        Skip();
        if (p < e && *p == ']') return ++p, true;
        while (p < e) {
            out.a.emplace_back();
            if (!Parse(out.a.back(), depth + 1)) return false;
            Skip();
            if (p < e && *p == ',') {
                ++p;
                continue;
            }
            if (p < e && *p == ']') return ++p, true;
            return false;
        }
        return false;
    }
    bool Obj(J& out, int depth) {
        out.type = J::kObj;
        ++p;
        Skip();
        if (p < e && *p == '}') return ++p, true;
        while (p < e) {
            Skip();
            std::string key;
            if (p >= e || *p != '"' || !Str(key)) return false;
            Skip();
            if (p >= e || *p != ':') return false;
            ++p;
            out.o.emplace_back(std::move(key), J{});
            if (!Parse(out.o.back().second, depth + 1)) return false;
            Skip();
            if (p < e && *p == ',') {
                ++p;
                continue;
            }
            if (p < e && *p == '}') return ++p, true;
            return false;
        }
        return false;
    }
};

bool ParseJson(const std::string& text, J& out) {
    Reader r(text.data(), text.data() + text.size());
    return r.Parse(out);
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ProcPath(const wchar_t* exe) { // the running copy's full path, empty when it isn't running
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe{sizeof(pe)};
    std::wstring path;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && path.empty(); ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t buf[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) path.assign(buf, len);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return path;
}

bool Spawn(const std::wstring& cmd, HANDLE out = nullptr, HANDLE* proc = nullptr) { // hidden: no console flashes up
    STARTUPINFOW si{sizeof(si)};
    if (out) si.dwFlags = STARTF_USESTDHANDLES, si.hStdOutput = out, si.hStdError = out;
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, out != nullptr, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    if (proc) *proc = pi.hProcess;
    else CloseHandle(pi.hProcess);
    return true;
}

std::string Capture(const std::wstring& cmd) { // a command's whole output
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); // only the child's end is inherited
    HANDLE proc = nullptr;
    bool ok = Spawn(cmd, wr, &proc);
    CloseHandle(wr);
    std::string out;
    if (ok) {
        char buf[16384];
        DWORD got = 0;
        while (out.size() < kMaxJson && ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
        WaitForSingleObject(proc, kCaptureMs);
        CloseHandle(proc);
    }
    CloseHandle(rd);
    return out;
}

std::wstring Quote(const std::wstring& path) { return L"\"" + path + L"\""; }

bool Busy(const J& ring) { // a list, or a ring of {elements, focused}, with anything in it
    if (ring.type == J::kArr) return !ring.a.empty();
    return ring["elements"].type == J::kArr && !ring["elements"].a.empty();
}

size_t Scan(const std::string& s, size_t end, size_t* begin) { // just past the last top-level object closing before end, 0 if none; begin: where it opened
    int depth = 0;
    bool str = false, esc = false;
    size_t last = 0, open = 0, lastOpen = 0;
    for (size_t i = 0; i < end; ++i) {
        char c = s[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{' && depth++ == 0) open = i;
        else if (c == '}' && depth > 0 && --depth == 0) last = i + 1, lastOpen = open;
    }
    if (begin) *begin = lastOpen;
    return last;
}

}

struct Workspaces::Impl {
    HWND hwnd = nullptr;
    UINT msg = 0;
    HANDLE stop = nullptr;
    std::thread worker;
    mutable std::mutex mu;
    WsState state;
    std::mutex sendMu; // guards the three below: the ui thread reads them to switch
    std::wstring cli; // the pipe manager's command line tool, while it runs
    HINTERNET socket = nullptr; // the socket manager's connection, while open
    std::vector<std::wstring> wsNames; // the socket manager's own names, by position: what its focus command takes
    std::vector<int> cliSlots; // the command line manager's workspace numbers, by position
    std::atomic<HANDLE> cliRead{nullptr}, cliProc{nullptr}; // its subscription: stopping ends the child, which wakes the read

    void Publish(WsState next) {
        {
            std::lock_guard<std::mutex> lock(mu);
            bool same = next.source == state.source && next.current == state.current && next.list.size() == state.list.size();
            for (size_t i = 0; same && i < next.list.size(); ++i) same = next.list[i].name == state.list[i].name && next.list[i].busy == state.list[i].busy;
            if (same) return;
            state = std::move(next);
        }
        PostMessageW(hwnd, msg, 0, 0);
    }

    bool Stopping() const { return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; }

    void Run() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // StringFromGUID2 needs none, winhttp none: kept for safety
        while (!Stopping()) {
            std::wstring tiling = ProcPath(kTilingExe);
            if (!tiling.empty()) RunTiling(tiling);
            else if (!ProcPath(kTilingWsExe).empty()) RunTilingWs();
            else if (std::wstring cliExe = ProcPath(kTilingCliExe); !cliExe.empty()) RunTilingCli(cliExe);
            else RunDesktops();
            if (WaitForSingleObject(stop, kRetryMs) == WAIT_OBJECT_0) break;
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }

    // ---- windows' virtual desktops: read from the registry, which says when they change ----
    WsState ReadDesktops() {
        WsState s;
        s.source = WsState::kDesktops;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kDesksKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            std::vector<GUID> ids(kMaxSpaces);
            DWORD bytes = (DWORD)(ids.size() * sizeof(GUID));
            if (RegQueryValueExW(key, L"VirtualDesktopIDs", nullptr, nullptr, (BYTE*)ids.data(), &bytes) == ERROR_SUCCESS) ids.resize(bytes / sizeof(GUID));
            else ids.clear();
            GUID cur{};
            DWORD cb = sizeof(cur);
            bool haveCur = RegQueryValueExW(key, L"CurrentVirtualDesktop", nullptr, nullptr, (BYTE*)&cur, &cb) == ERROR_SUCCESS && cb == sizeof(cur);
            for (size_t i = 0; i < ids.size(); ++i) {
                wchar_t g[64];
                StringFromGUID2(ids[i], g, 64);
                wchar_t name[256] = {};
                DWORD nb = sizeof(name);
                std::wstring sub = std::wstring(L"Desktops\\") + g;
                if (RegGetValueW(key, sub.c_str(), L"Name", RRF_RT_REG_SZ, nullptr, name, &nb) != ERROR_SUCCESS || !name[0]) swprintf_s(name, L"%zu", i + 1);
                s.list.push_back({name, true});
                if (haveCur && IsEqualGUID(ids[i], cur)) s.current = (int)i;
            }
            RegCloseKey(key);
        }
        if (s.list.empty()) s.list.push_back({L"1", true}), s.current = 0; // one desktop: windows writes nothing until a second exists
        if (s.current < 0) s.current = 0;
        return s;
    }

    void RunDesktops() {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kDesksKey, 0, nullptr, 0, KEY_NOTIFY | KEY_READ, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            Publish(ReadDesktops());
            WaitForSingleObject(stop, kLookMs);
            return;
        }
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Publish(ReadDesktops());
        for (;;) {
            RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, ev, TRUE);
            HANDLE waits[2] = {stop, ev};
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, kLookMs);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) Publish(ReadDesktops());
            else if (!ProcPath(kTilingExe).empty() || !ProcPath(kTilingWsExe).empty() || !ProcPath(kTilingCliExe).empty()) break; // a tiling manager started: it takes over
        }
        CloseHandle(ev);
        RegCloseKey(key);
    }

    // ---- the pipe manager: its state once, then every change pushed down a pipe of ours ----
    void TakeTiling(const J& doc) {
        const J& root = doc["state"].type == J::kObj ? doc["state"] : doc;
        const J& mons = root["monitors"];
        const J& mon = mons["elements"].At((size_t)(std::max)(0, mons["focused"].Int()));
        const J& wss = mon["workspaces"];
        const J& list = wss["elements"];
        if (list.type != J::kArr) return;
        WsState s;
        s.source = WsState::kTiling;
        s.current = wss["focused"].Int(-1);
        for (size_t i = 0; i < list.a.size() && (int)i < kMaxSpaces; ++i) {
            const J& w = list.a[i];
            std::wstring name = w["name"].type == J::kStr && !w["name"].s.empty() ? Wide(w["name"].s) : std::to_wstring(i + 1);
            bool busy = Busy(w["containers"]) || w["monocle_container"].type == J::kObj || w["maximized_window"].type == J::kObj || Busy(w["floating_windows"]);
            s.list.push_back({name, busy});
        }
        Publish(std::move(s));
    }

    void RunTiling(const std::wstring& exe) {
        std::wstring cliPath = exe.substr(0, exe.find_last_of(L"\\/") + 1) + kTilingCli;
        if (GetFileAttributesW(cliPath.c_str()) == INVALID_FILE_ATTRIBUTES) cliPath = kTilingCli; // elsewhere on the path
        {
            std::lock_guard<std::mutex> lock(sendMu);
            cli = cliPath;
        }
        J doc;
        if (ParseJson(Capture(Quote(cliPath) + L" state"), doc)) TakeTiling(doc);
        std::wstring name = L"astraeus-ws-" + std::to_wstring(GetCurrentProcessId());
        std::wstring path = L"\\\\.\\pipe\\" + name;
        HANDLE pipe = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 1 << 20, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL now = ConnectNamedPipe(pipe, &ov);
            DWORD err = now ? ERROR_PIPE_CONNECTED : GetLastError();
            Spawn(Quote(cliPath) + L" subscribe-pipe " + name); // it connects to us and writes every change
            HANDLE waits[2] = {stop, ov.hEvent};
            bool connected = err == ERROR_PIPE_CONNECTED || (err == ERROR_IO_PENDING && WaitForMultipleObjects(2, waits, FALSE, kConnectMs) == WAIT_OBJECT_0 + 1);
            if (!connected) CancelIo(pipe);
            std::string buf;
            std::vector<char> chunk(1 << 16);
            while (connected && !Stopping()) {
                ResetEvent(ov.hEvent);
                DWORD got = 0;
                if (!ReadFile(pipe, chunk.data(), (DWORD)chunk.size(), &got, &ov)) {
                    if (GetLastError() != ERROR_IO_PENDING) break;
                    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                        CancelIo(pipe);
                        break;
                    }
                    if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) break; // the manager quit
                }
                buf.append(chunk.data(), got);
                if (buf.size() > kMaxJson) buf.clear();
                size_t begin = 0, end = Scan(buf, buf.size(), &begin); // only the newest whole state matters
                if (end) {
                    J next;
                    if (ParseJson(buf.substr(begin, end - begin), next)) TakeTiling(next);
                    buf.erase(0, end);
                }
            }
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
        }
        std::lock_guard<std::mutex> lock(sendMu);
        cli.clear();
    }

    // ---- the command line manager: one line of json per change from its subscribe command, the whole state asked for again on each ----
    void TakeTilingCli(const J& doc) {
        const J& root = doc["records"].type == J::kArr ? doc : doc["snapshot"].type == J::kObj ? doc["snapshot"] : doc["data"];
        const J& records = root["records"];
        if (records.type != J::kArr) return;
        std::string mon = root["focused_monitor_device_name"].s;
        int active = 0;
        for (const J& r : records.a)
            if (r["kind"].s == "monitor" && (mon.empty() || r["monitor_device_name"].s == mon)) {
                if (mon.empty()) mon = r["monitor_device_name"].s; // none focused yet: the first screen speaks
                active = r["active_workspace_index"].Int();
            }
        WsState s;
        s.source = WsState::kTilingCli;
        std::vector<int> slots;
        for (const J& r : records.a) {
            if (r["kind"].s != "workspace" || r["monitor_device_name"].s != mon || (int)slots.size() >= kMaxSpaces) continue;
            int at = r["workspace_index"].Int();
            std::wstring name = r["name"].type == J::kStr && !r["name"].s.empty() ? Wide(r["name"].s) : std::to_wstring(at + 1);
            bool busy = std::any_of(records.a.begin(), records.a.end(), [&](const J& w) { return w["kind"].s == "window" && w["monitor_device_name"].s == mon && w["workspace_index"].Int() == at; });
            if (at == active) s.current = (int)s.list.size();
            slots.push_back(at);
            s.list.push_back({name, busy});
        }
        {
            std::lock_guard<std::mutex> lock(sendMu);
            cliSlots = std::move(slots);
        }
        Publish(std::move(s));
    }

    void RunTilingCli(const std::wstring& exe) {
        std::wstring tool = exe.substr(0, exe.find_last_of(L"\\/") + 1) + kTilingCliTool;
        if (GetFileAttributesW(tool.c_str()) == INVALID_FILE_ATTRIBUTES) tool = kTilingCliTool; // elsewhere on the path
        {
            std::lock_guard<std::mutex> lock(sendMu);
            cli = tool;
        }
        auto ask = [&] {
            J doc;
            if (ParseJson(Capture(Quote(tool) + L" query workspaces"), doc)) TakeTilingCli(doc);
        };
        ask();
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        HANDLE proc = nullptr;
        if (CreatePipe(&rd, &wr, &sa, 0)) {
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            bool ok = Spawn(Quote(tool) + L" subscribe --events workspace_state", wr, &proc);
            CloseHandle(wr);
            cliRead = rd; // Stop closes the child, which ends the blocking read below
            cliProc = proc;
            char buf[4096];
            DWORD got = 0;
            while (ok && !Stopping() && ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got)
                if (memchr(buf, '\n', got)) ask(); // a frame ended: something moved
            cliRead = nullptr;
            cliProc = nullptr;
            if (proc) {
                TerminateProcess(proc, 0); // the subscription, if it's still there
                CloseHandle(proc);
            }
            CloseHandle(rd);
        }
        std::lock_guard<std::mutex> lock(sendMu);
        cli.clear();
        cliSlots.clear();
    }

    // ---- the socket manager: a websocket on localhost, asked again on each event ----
    bool Send(const std::string& text) {
        std::lock_guard<std::mutex> lock(sendMu);
        return socket && WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(), (DWORD)text.size()) == NO_ERROR;
    }

    void TakeTilingWs(const J& doc) {
        const J& list = doc["data"]["workspaces"];
        if (list.type != J::kArr) return;
        struct Named {
            std::wstring id;
            Workspace ws;
        };
        std::vector<Named> named;
        std::wstring focused;
        for (const auto& w : list.a) {
            std::wstring id = Wide(w["name"].s);
            std::wstring shown = w["displayName"].type == J::kStr && !w["displayName"].s.empty() ? Wide(w["displayName"].s) : id;
            named.push_back({id, {shown, Busy(w["children"])}});
            if (w["hasFocus"].b) focused = id;
        }
        std::stable_sort(named.begin(), named.end(), [](const Named& a, const Named& b) { // 1, 2, 10 rather than 1, 10, 2
            int x = _wtoi(a.id.c_str()), y = _wtoi(b.id.c_str());
            return x != y ? x < y : a.id < b.id;
        });
        WsState s;
        s.source = WsState::kTilingWs;
        std::vector<std::wstring> ids;
        for (size_t i = 0; i < named.size() && (int)i < kMaxSpaces; ++i) {
            s.list.push_back(named[i].ws);
            ids.push_back(named[i].id);
            if (named[i].id == focused) s.current = (int)i;
        }
        {
            std::lock_guard<std::mutex> lock(sendMu);
            wsNames = std::move(ids);
        }
        Publish(std::move(s));
    }

    void RunTilingWs() {
        HINTERNET session = WinHttpOpen(L"Astraeus", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET conn = session ? WinHttpConnect(session, L"localhost", kTilingWsPort, 0) : nullptr;
        HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
        HINTERNET ws = nullptr;
        if (req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr))
            ws = WinHttpWebSocketCompleteUpgrade(req, 0);
        if (req) WinHttpCloseHandle(req);
        if (ws) {
            {
                std::lock_guard<std::mutex> lock(sendMu);
                socket = ws;
            }
            Send("sub --events workspace_activated workspace_deactivated workspace_updated focus_changed");
            Send("query workspaces");
            std::string text;
            std::vector<char> chunk(1 << 16);
            while (!Stopping()) {
                DWORD got = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                if (WinHttpWebSocketReceive(ws, chunk.data(), (DWORD)chunk.size(), &got, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                text.append(chunk.data(), got);
                if (text.size() > kMaxJson) text.clear();
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
                J doc;
                if (ParseJson(text, doc)) {
                    if (doc["messageType"].s == "event_subscription") Send("query workspaces"); // something moved: ask for the whole list again
                    else if (doc["data"]["workspaces"].type == J::kArr) TakeTilingWs(doc);
                }
                text.clear();
            }
            {
                std::lock_guard<std::mutex> lock(sendMu);
                socket = nullptr;
            }
            WinHttpCloseHandle(ws);
        }
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }
};

namespace {

Tiler RunningTiler() { // one look at the process list, however many managers there are to ask about
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return Tiler::kNone;
    Tiler seen = Tiler::kNone;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        Tiler t = !_wcsicmp(pe.szExeFile, kTilingExe) ? Tiler::kPipe : !_wcsicmp(pe.szExeFile, kTilingWsExe) ? Tiler::kSocket : !_wcsicmp(pe.szExeFile, kTilingCliExe) ? Tiler::kCli
                : !_wcsicmp(pe.szExeFile, kFancyExe) ? Tiler::kFancy : !_wcsicmp(pe.szExeFile, kAmethystExe) ? Tiler::kAmethyst : Tiler::kNone;
        if (t != Tiler::kNone && (seen == Tiler::kNone || (int)t < (int)seen)) seen = t; // the order the worker prefers them in
    }
    CloseHandle(snap);
    return seen;
}

bool StorePackage(const wchar_t* pattern) { // a store-installed app keeps a folder under the user's packages
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((std::wstring(base) + L"\\Packages\\" + pattern).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    FindClose(f);
    return true;
}

}

Tiler FindTiler() {
    if (Tiler running = RunningTiler(); running != Tiler::kNone) return running;
    wchar_t found[MAX_PATH];
    auto onPath = [&](const wchar_t* exe) { return SearchPathW(nullptr, exe, nullptr, MAX_PATH, found, nullptr) != 0; }; // scoop, winget and cargo all put them there
    if (onPath(kTilingCli)) return Tiler::kPipe;
    if (onPath(kTilingWsExe)) return Tiler::kSocket;
    if (onPath(kTilingCliTool)) return Tiler::kCli;
    wchar_t pf[MAX_PATH] = {}, home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    auto has = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
    if (has(std::wstring(pf) + L"\\komorebi\\bin\\" + kTilingExe) || has(std::wstring(home) + L"\\scoop\\shims\\" + kTilingExe)) return Tiler::kPipe;
    if (has(std::wstring(pf) + L"\\glzr.io\\GlazeWM\\" + kTilingWsExe) || has(std::wstring(home) + L"\\scoop\\shims\\" + kTilingWsExe)) return Tiler::kSocket;
    if (has(std::wstring(pf) + L"\\LeopardWM\\" + kTilingCliExe) || has(std::wstring(home) + L"\\scoop\\shims\\" + kTilingCliExe)) return Tiler::kCli;
    if (StorePackage(L"*FancyWM*") || onPath(kFancyExe)) return Tiler::kFancy;
    if (StorePackage(L"*Amethyst*") || onPath(kAmethystExe)) return Tiler::kAmethyst;
    return Tiler::kNone;
}

const wchar_t* TilerName(Tiler t) {
    switch (t) {
    case Tiler::kPipe: return L"Komorebi";
    case Tiler::kSocket: return L"GlazeWM";
    case Tiler::kCli: return L"LeopardWM";
    case Tiler::kFancy: return L"FancyWM";
    case Tiler::kAmethyst: return L"AmethystWindows";
    default: return L"Windows desktops";
    }
}

Workspaces::Workspaces() = default;
Workspaces::~Workspaces() { Stop(); }

void Workspaces::Start(HWND notify, UINT msg) {
    if (impl) return;
    impl = std::make_shared<Impl>();
    impl->hwnd = notify, impl->msg = msg;
    impl->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto self = impl; // the worker keeps it alive until it returns
    impl->worker = std::thread([self] { self->Run(); });
}

void Workspaces::Stop() {
    if (!impl) return;
    SetEvent(impl->stop);
    {
        std::lock_guard<std::mutex> lock(impl->sendMu);
        if (impl->socket) WinHttpWebSocketClose(impl->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); // wakes a blocked receive
    }
    if (HANDLE sub = impl->cliProc.load()) TerminateProcess(sub, 0); // its pipe closes: the worker's read returns
    if (impl->worker.joinable()) impl->worker.join();
    CloseHandle(impl->stop);
    impl.reset();
}

bool Workspaces::On() const { return impl != nullptr; }

WsState Workspaces::Snap() const {
    if (!impl) return {};
    std::lock_guard<std::mutex> lock(impl->mu);
    return impl->state;
}

void Workspaces::Focus(int index) {
    if (!impl) return;
    WsState s = Snap();
    if (index < 0 || index >= (int)s.list.size() || index == s.current) return;
    if (s.source == WsState::kTiling) {
        std::wstring tool;
        {
            std::lock_guard<std::mutex> lock(impl->sendMu);
            tool = impl->cli;
        }
        if (!tool.empty()) Spawn(Quote(tool) + L" focus-workspace " + std::to_wstring(index));
    } else if (s.source == WsState::kTilingCli) {
        std::wstring tool;
        int slot = -1;
        {
            std::lock_guard<std::mutex> lock(impl->sendMu);
            tool = impl->cli;
            if (index < (int)impl->cliSlots.size()) slot = impl->cliSlots[index];
        }
        if (!tool.empty() && slot >= 0) Spawn(Quote(tool) + L" workspace " + std::to_wstring(slot + 1)); // it counts from one
    } else if (s.source == WsState::kTilingWs) {
        std::wstring id;
        {
            std::lock_guard<std::mutex> lock(impl->sendMu);
            if (index < (int)impl->wsNames.size()) id = impl->wsNames[index];
        }
        if (!id.empty()) impl->Send("command focus --workspace " + Narrow(id));
    } else if (s.source == WsState::kDesktops && s.current >= 0) { // windows has no public call for it: the keys a person would press
        int delta = index - s.current;
        WORD arrow = delta > 0 ? VK_RIGHT : VK_LEFT;
        std::vector<INPUT> keys;
        auto key = [&](WORD vk, bool up) {
            INPUT in{INPUT_KEYBOARD};
            in.ki.wVk = vk;
            in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_LWIN ? KEYEVENTF_EXTENDEDKEY : 0);
            keys.push_back(in);
        };
        key(VK_LCONTROL, false), key(VK_LWIN, false);
        for (int i = 0; i < std::abs(delta); ++i) key(arrow, false), key(arrow, true);
        key(VK_LWIN, true), key(VK_LCONTROL, true);
        SendInput((UINT)keys.size(), keys.data(), sizeof(INPUT));
    }
}

void Workspaces::Step(int delta) {
    WsState s = Snap();
    int n = (int)s.list.size();
    if (n < 2 || s.current < 0) return;
    Focus(((s.current + delta) % n + n) % n);
}

}
