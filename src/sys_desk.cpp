#include "desk.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <sstream>

namespace sys {
namespace {

constexpr wchar_t kKey[] = L"Software\\Astraeus", kValue[] = L"MixDesk", kLock[] = L"Local\\AstraeusMixDesk"; // shared with halobar: one setup, one engine
constexpr float kLoud = 0.02f, kHold = 1.2f; // a lowering group counts as playing above this peak, and for this long after
constexpr float kDuckDown = 8, kDuckUp = 1.6f; // per second: ducks fast, comes back gently
constexpr float kDrift = 0.02f, kStep = 0.004f; // a level this far from ours was moved by someone else; closer than a step isn't worth a write
constexpr uint32_t kSwatches[Desk::kColors] = {0x7C9CFF, 0xFF7A90, 0x5ED6A0, 0xFFB454, 0xB98CFF, 0x4FD1E8, 0xFF8F5E, 0xE6E6EA};

std::wstring Clean(std::wstring s) { // names live inside a tab and line format
    for (auto& c : s)
        if (c == L'\t' || c == L'\n' || c == L'\r' || c == L'|' || c == L';' || c == L',') c = L' ';
    return s;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    for (size_t b; (b = s.find(sep, a)) != std::wstring::npos; a = b + 1) out.push_back(s.substr(a, b - a));
    out.push_back(s.substr(a));
    return out;
}

float Num(const std::wstring& s, float def) {
    wchar_t* end = nullptr;
    float v = std::wcstof(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

std::wstring Fmt(float v) {
    wchar_t b[16];
    swprintf_s(b, L"%.3f", v);
    return b;
}

std::wstring last; // the text last read or written here: our own writes don't come back as changes

std::wstring ReadText() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, kValue, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, kValue, RRF_RT_REG_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

}

uint32_t Desk::Swatch(int k) { return kSwatches[((k % kColors) + kColors) % kColors]; }

int Desk::GroupOf(const std::wstring& key) const {
    for (int g = 0; g < (int)groups.size(); ++g)
        for (const auto& a : groups[g].apps)
            if (a == key) return g;
    return -1;
}

float Desk::ChatGain(int g) const {
    if (!Chatting()) return 1;
    if (g == game) return chatMix < 0 ? 1 + chatMix : 1; // toward chat, the game fades
    if (g == chat) return chatMix > 0 ? 1 - chatMix : 1;
    return 1;
}

void Desk::Assign(const std::wstring& key, int g) {
    for (auto& grp : groups) grp.apps.erase(std::remove(grp.apps.begin(), grp.apps.end(), key), grp.apps.end());
    if (g >= 0 && g < (int)groups.size()) groups[g].apps.push_back(key);
}

int Desk::AddGroup() {
    if ((int)groups.size() >= kMaxGroups) return -1;
    int n = (int)groups.size();
    DeskGroup grp;
    grp.name = L"Group " + std::to_wstring(n + 1);
    grp.color = Swatch(n);
    grp.icon = n % kIcons;
    groups.push_back(std::move(grp));
    return n;
}

void Desk::RemoveGroup(int g) {
    if (g < 0 || g >= (int)groups.size()) return;
    groups.erase(groups.begin() + g);
    auto shift = [g](int& i) { i = i == g ? -1 : i > g ? i - 1 : i; };
    shift(game);
    shift(chat);
    for (auto& p : profiles) {
        if (g < (int)p.volumes.size()) p.volumes.erase(p.volumes.begin() + g);
        if (g < (int)p.mutes.size()) p.mutes.erase(p.mutes.begin() + g);
        if (g < (int)p.devices.size()) p.devices.erase(p.devices.begin() + g);
    }
}

void Desk::SaveProfile(int slot, const std::wstring& name) {
    if (slot < 0 || slot > (int)profiles.size() || slot >= kMaxProfiles) return;
    if (slot == (int)profiles.size()) profiles.emplace_back();
    DeskProfile& p = profiles[slot];
    p.name = name.empty() ? L"Profile " + std::to_wstring(slot + 1) : Clean(name);
    p.chatMix = chatMix;
    p.volumes.clear(), p.mutes.clear(), p.devices.clear();
    for (const auto& grp : groups) p.volumes.push_back(grp.volume), p.mutes.push_back(grp.muted), p.devices.push_back(grp.device);
    profile = slot;
}

void Desk::Recall(int slot) {
    if (slot < 0 || slot >= (int)profiles.size()) return;
    const DeskProfile& p = profiles[slot];
    chatMix = p.chatMix;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g < p.volumes.size()) groups[g].volume = p.volumes[g];
        if (g < p.mutes.size()) groups[g].muted = p.mutes[g] != 0;
        if (g < p.devices.size()) groups[g].device = p.devices[g];
    }
    profile = slot;
}

std::wstring Desk::Write() const { // one line per thing, tab separated; lists inside use | then ; and ,
    std::wostringstream o;
    o << L"v1\n";
    for (const auto& g : groups) {
        wchar_t col[8];
        swprintf_s(col, L"%06X", g.color & 0xFFFFFF);
        o << L"G\t" << Clean(g.name) << L'\t' << col << L'\t' << g.icon << L'\t' << Fmt(g.volume) << L'\t' << g.muted << L'\t' << g.ducks << L'\t' << g.lowers << L'\t' << g.device << L'\t';
        for (size_t k = 0; k < g.apps.size(); ++k) o << (k ? L"|" : L"") << g.apps[k];
        o << L'\n';
    }
    o << L"C\t" << game << L'\t' << chat << L'\t' << Fmt(chatMix) << L'\n';
    o << L"O\t" << duckBy << L'\t' << keys << L'\t' << osd << L'\t' << profile << L'\n';
    for (const auto& [k, v] : base) o << L"B\t" << k << L'\t' << Fmt(v) << L'\n';
    for (const auto& p : profiles) {
        o << L"P\t" << Clean(p.name) << L'\t' << Fmt(p.chatMix) << L'\t';
        for (size_t g = 0; g < p.volumes.size(); ++g)
            o << (g ? L";" : L"") << Fmt(p.volumes[g]) << L',' << (g < p.mutes.size() ? (int)p.mutes[g] : 0) << L',' << (g < p.devices.size() ? p.devices[g] : L"");
        o << L'\n';
    }
    return o.str();
}

void Desk::Read(const std::wstring& text) {
    *this = Desk{};
    for (const auto& line : Split(text, L'\n')) {
        auto f = Split(line, L'\t');
        if (f.empty() || f[0].empty()) continue;
        if (f[0] == L"G" && f.size() >= 10 && (int)groups.size() < kMaxGroups) {
            DeskGroup g;
            g.name = f[1];
            g.color = wcstoul(f[2].c_str(), nullptr, 16) & 0xFFFFFF;
            g.icon = (std::clamp)((int)Num(f[3], 0), 0, kIcons - 1);
            g.volume = (std::clamp)(Num(f[4], 1), 0.0f, 1.0f);
            g.muted = f[5] == L"1", g.ducks = f[6] == L"1", g.lowers = f[7] == L"1";
            g.device = f[8];
            for (auto& a : Split(f[9], L'|'))
                if (!a.empty()) g.apps.push_back(a);
            groups.push_back(std::move(g));
        } else if (f[0] == L"C" && f.size() >= 4) {
            game = (int)Num(f[1], -1), chat = (int)Num(f[2], -1);
            chatMix = (std::clamp)(Num(f[3], 0), -1.0f, 1.0f);
        } else if (f[0] == L"O" && f.size() >= 5) {
            duckBy = (std::clamp)((int)Num(f[1], kDuckDef), kDuckMin, kDuckMax);
            keys = f[2] == L"1", osd = f[3] == L"1";
            profile = (int)Num(f[4], -1);
        } else if (f[0] == L"B" && f.size() >= 3) {
            base[f[1]] = (std::clamp)(Num(f[2], 1), 0.0f, 1.0f);
        } else if (f[0] == L"P" && f.size() >= 4 && (int)profiles.size() < kMaxProfiles) {
            DeskProfile p;
            p.name = f[1];
            p.chatMix = (std::clamp)(Num(f[2], 0), -1.0f, 1.0f);
            if (!f[3].empty())
                for (auto& slot : Split(f[3], L';')) {
                    auto v = Split(slot, L',');
                    p.volumes.push_back((std::clamp)(Num(v[0], 1), 0.0f, 1.0f));
                    p.mutes.push_back(v.size() > 1 && v[1] == L"1");
                    p.devices.push_back(v.size() > 2 ? v[2] : L"");
                }
            profiles.push_back(std::move(p));
        }
    }
    auto valid = [&](int& i) { if (i < -1 || i >= (int)groups.size()) i = -1; };
    valid(game), valid(chat);
    if (profile >= (int)profiles.size()) profile = -1;
}

Desk LoadDesk() {
    Desk d;
    last = ReadText();
    d.Read(last);
    return d;
}

void SaveDesk(const Desk& d) {
    std::wstring text = d.Write();
    if (text == last) return; // nothing new: no write, no wake for the other app
    last = text;
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, kValue, REG_SZ, text.c_str(), (DWORD)((text.size() + 1) * sizeof(wchar_t)));
}

bool DeskChanged() {
    static HKEY key = nullptr;
    static HANDLE ev = nullptr;
    if (!key) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_READ | KEY_NOTIFY, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE); // windows signals, nothing reads the registry until then
        return false;
    }
    if (!ev || WaitForSingleObject(ev, 0) != WAIT_OBJECT_0) return false;
    RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE);
    std::wstring now = ReadText();
    if (now == last) return false; // some other value under the key
    last = now;
    return true;
}

DeskEngine::~DeskEngine() {
    if (lock) CloseHandle(lock);
}

bool DeskEngine::Claim() {
    if (lock) return true;
    HANDLE h = CreateMutexW(nullptr, TRUE, kLock);
    if (!h) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { // the other app holds it: it applies, this one edits
        CloseHandle(h);
        return false;
    }
    lock = h;
    return true;
}

bool DeskEngine::NeedsMeter(const Desk& d) const {
    bool lowers = false, ducks = false;
    for (const auto& g : d.groups) lowers |= g.lowers && !g.apps.empty(), ducks |= g.ducks && !g.apps.empty();
    return Owner() && lowers && ducks;
}

void DeskEngine::Tick(Mixer& mix, Desk& d, float dt) {
    if (!Owner() || !mix.IsOpen()) return;
    bool listen = NeedsMeter(d), loud = false;
    if (listen)
        for (const auto& a : mix.Apps()) {
            int g = d.GroupOf(a.key);
            loud |= g >= 0 && d.groups[g].lowers && !d.groups[g].muted && !a.muted && a.peak > kLoud;
        }
    quiet = loud ? 0 : quiet + dt;
    float want = listen && quiet < kHold ? 1 - d.duckBy / 100.0f : 1;
    duck += (want - duck) * (std::min)(1.0f, dt * (want < duck ? kDuckDown : kDuckUp));
    if (std::fabs(duck - want) < 0.005f) duck = want;

    std::vector<MixApp> apps = mix.Apps(); // a copy: setting levels updates the mixer's rows
    for (const auto& a : apps) {
        int g = d.GroupOf(a.key);
        if (g < 0) { // left its group: hand back what we took
            if (mutedByUs.erase(a.key)) mix.SetMuted(a.key, false);
            if (routedByUs.erase(a.key) && !a.route.empty()) mix.Route(a.key, L"");
            written.erase(a.key);
            continue;
        }
        const DeskGroup& grp = d.groups[g];
        float factor = grp.volume * d.ChatGain(g) * (grp.ducks ? duck : 1);
        auto w = written.find(a.key);
        auto b = d.base.find(a.key);
        if (b == d.base.end()) b = d.base.emplace(a.key, a.volume).first; // first seen: its level so far is its own
        else if (w != written.end() && std::fabs(a.volume - w->second) > kDrift && factor > 0.01f) b->second = (std::min)(1.0f, a.volume / factor); // moved elsewhere: keep it
        float target = (std::clamp)(b->second * factor, 0.0f, 1.0f);
        bool write = std::fabs(target - a.volume) > kStep;
        if (write) mix.SetVolume(a.key, target);
        written[a.key] = write ? target : a.volume;
        if (grp.muted && !a.muted) mix.SetMuted(a.key, true), mutedByUs.insert(a.key);
        else if (!grp.muted && a.muted && mutedByUs.erase(a.key)) mix.SetMuted(a.key, false);
        if (!grp.device.empty() && a.route != grp.device) {
            if (mix.Route(a.key, grp.device)) routedByUs.insert(a.key);
        } else if (grp.device.empty() && !a.route.empty() && routedByUs.erase(a.key)) {
            mix.Route(a.key, L"");
        }
    }
}

}
