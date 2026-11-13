#include "sys.h"

namespace sys {

namespace {
constexpr wchar_t kRegKey[] = L"Software\\Astraeus"; // HKCU settings root
}

bool ReadFlag(const wchar_t* name, bool fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) // missing or wrong type
        return fallback;
    return value != 0;
}

std::wstring ClipText() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.assign(p, wcsnlen(p, 64)); // a color code is short, ignore the rest
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

int ReadNum(const wchar_t* name, int fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return fallback;
    return (int)value;
}

void SaveNum(const wchar_t* name, int v) {
    DWORD value = (DWORD)v;
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &value, sizeof(value)); // creates the key if missing
}

void SaveFlag(const wchar_t* name, bool on) { SaveNum(name, on ? 1 : 0); }

std::wstring ReadText(const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring out((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    DWORD bufBytes = (DWORD)(out.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, out.data(), &bufBytes) != ERROR_SUCCESS) return {};
    out.resize(wcsnlen(out.c_str(), out.size()));
    return out;
}

void SaveText(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

}
