#include "backup.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cwctype>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace backup {

namespace {

constexpr wchar_t kKey[] = L"Software\\Astraeus";
constexpr wchar_t kSettings[] = L"[Settings]", kConfig[] = L"[Config]";
constexpr const wchar_t* kSkip[] = {L"UpdatedTo", L"WxPlace", L"WxCityPlace"}; // the updater's own state and where you are: not settings, never exported
constexpr DWORD kTextMax = 1024; // longer text values aren't settings
constexpr LONGLONG kFileMax = 256 << 10;

std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring o(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n);
    return o;
}

bool ReadAll(const std::wstring& path, std::wstring& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < kFileMax;
    if (ok) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr) && got == bytes.size();
    }
    CloseHandle(f);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF) bytes.erase(0, 3); // a bom from notepad
    out = Wide(bytes);
    return ok;
}

bool WriteAll(const std::wstring& path, const std::wstring& text) {
    std::string bytes = Utf8(text);
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE; // whole or not at all
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

bool Skipped(const wchar_t* name) {
    for (const wchar_t* s : kSkip)
        if (!_wcsicmp(s, name)) return true;
    return false;
}

bool PlainName(const std::wstring& n) { // value names astraeus writes: letters and digits only
    if (n.empty() || n.size() > 64) return false;
    for (wchar_t c : n)
        if (!iswalnum(c)) return false;
    return true;
}

std::wstring Pick(HWND owner, bool save) { // windows' own file dialog, .ini only
    ComPtr<IFileDialog> dlg;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return L"";
    COMDLG_FILTERSPEC types[] = {{L"Astraeus settings", L"*.ini"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"ini");
    dlg->SetTitle(save ? L"Export Astraeus settings" : L"Import Astraeus settings");
    if (save) dlg->SetFileName(L"Astraeus settings.ini");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

}

bool Export(HWND owner, const std::wstring& iniPath) {
    std::wstring file = Pick(owner, true);
    return !file.empty() && ExportTo(file, iniPath, kKey);
}

bool Import(HWND owner, const std::wstring& iniPath) {
    std::wstring file = Pick(owner, false);
    return !file.empty() && ImportFrom(file, iniPath, kKey);
}

bool ExportTo(const std::wstring& file, const std::wstring& iniPath, const wchar_t* regKey) {
    std::wstring text = L"# Astraeus settings. Import them from Settings, or keep them as a backup.\r\n\r\n";
    text += kSettings;
    text += L"\r\n";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, regKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t name[256];
            DWORD nameLen = 256, type = 0, size = 0;
            BYTE data[(kTextMax + 1) * sizeof(wchar_t)];
            size = sizeof(data);
            LSTATUS st = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data, &size);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st != ERROR_SUCCESS || Skipped(name) || !PlainName(name)) continue;
            if (type == REG_DWORD && size == sizeof(DWORD)) {
                text += std::wstring(name) + L" = " + std::to_wstring((int)*reinterpret_cast<DWORD*>(data)) + L"\r\n";
            } else if (type == REG_SZ) {
                std::wstring v(reinterpret_cast<wchar_t*>(data), wcsnlen(reinterpret_cast<wchar_t*>(data), size / sizeof(wchar_t)));
                if (v.find_first_of(L"\r\n\"") == std::wstring::npos) text += std::wstring(name) + L" = \"" + v + L"\"\r\n"; // quoted: it stays text
            }
        }
        RegCloseKey(key);
    }
    std::wstring ini;
    if (ReadAll(iniPath, ini)) { // the hand-edited config rides along, its keys only
        text += L"\r\n";
        text += kConfig;
        text += L"\r\n";
        for (size_t at = 0; at < ini.size();) {
            size_t end = ini.find(L'\n', at);
            if (end == std::wstring::npos) end = ini.size();
            std::wstring line = Trim(ini.substr(at, end - at));
            at = end + 1;
            if (!line.empty() && line[0] != L'#' && line[0] != L';' && line[0] != L'[' && line.find(L'=') != std::wstring::npos) text += line + L"\r\n";
        }
    }
    return WriteAll(file, text);
}

bool ImportFrom(const std::wstring& file, const std::wstring& iniPath, const wchar_t* regKey) {
    std::wstring all;
    if (!ReadAll(file, all)) return false;
    if (all.find(kSettings) == std::wstring::npos && all.find(kConfig) == std::wstring::npos) return false; // not one of ours
    int section = 0; // 1 settings, 2 config
    std::wstring config;
    bool any = false;
    for (size_t at = 0; at < all.size();) {
        size_t end = all.find(L'\n', at);
        if (end == std::wstring::npos) end = all.size();
        std::wstring line = Trim(all.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        if (line == kSettings) { section = 1; continue; }
        if (line == kConfig) { section = 2; continue; }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || !section) continue;
        if (section == 2) {
            config += line + L"\r\n";
            continue;
        }
        std::wstring name = Trim(line.substr(0, eq)), val = Trim(line.substr(eq + 1));
        if (!PlainName(name) || Skipped(name.c_str())) continue;
        if (val.size() >= 2 && val.front() == L'"' && val.back() == L'"') {
            val = val.substr(1, val.size() - 2);
            if (val.size() <= kTextMax) any |= RegSetKeyValueW(HKEY_CURRENT_USER, regKey, name.c_str(), REG_SZ, val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        } else {
            wchar_t* stop = nullptr;
            long n = wcstol(val.c_str(), &stop, 10);
            if (stop && !*stop && stop != val.c_str()) { // a whole number and nothing else
                DWORD v = (DWORD)n;
                any |= RegSetKeyValueW(HKEY_CURRENT_USER, regKey, name.c_str(), REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
            }
        }
    }
    if (!config.empty()) CopyFileW(iniPath.c_str(), (iniPath + L".bak").c_str(), FALSE); // the hand-edited file, comments and all, kept beside it
    if (!config.empty() && WriteAll(iniPath, L"# Astraeus config, imported. Right-click the tray icon > Restart after editing.\r\n\r\n" + config)) any = true;
    return any;
}

void Reset() {
    ResetKey(kKey); // every panel choice, the shelf's keep switch among them
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) DeleteFileW((std::wstring(base) + L"\\Astraeus\\shelf.txt").c_str()); // a kept shelf goes too
    CoTaskMemFree(base);
}

void ResetKey(const wchar_t* regKey) {
    struct Kept {
        const wchar_t* name;
        DWORD type, size;
        BYTE data[(kTextMax + 1) * sizeof(wchar_t)];
    };
    std::vector<Kept> kept; // the updater's state and the place found for you aren't settings: a reset keeps them, as export leaves them out
    for (const wchar_t* name : kSkip) {
        Kept k{name, 0, sizeof(k.data), {}};
        if (RegGetValueW(HKEY_CURRENT_USER, regKey, name, RRF_RT_REG_SZ | RRF_RT_REG_DWORD, &k.type, k.data, &k.size) == ERROR_SUCCESS) kept.push_back(k);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, regKey);
    for (const Kept& k : kept) RegSetKeyValueW(HKEY_CURRENT_USER, regKey, k.name, k.type, k.data, k.size);
}

}
