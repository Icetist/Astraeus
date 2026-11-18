#include "sys_toast.h"

#include <windows.h>

#include <cwctype>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

namespace toast {
namespace {

std::wstring Escape(const std::wstring& s) { // the words go inside xml
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::wstring Link(const std::wstring& action) { return std::wstring(kScheme) + L":" + action; }

}

bool Show(const std::wstring& title, const std::wstring& body, const std::vector<Button>& buttons, bool stay) {
    std::wstring xml = L"<toast activationType=\"protocol\" launch=\"" + Escape(Link(L"open")) + L"\"" + (stay ? L" scenario=\"reminder\"" : L"") + L">"; // a click on it opens the panel
    xml += L"<visual><binding template=\"ToastGeneric\"><text>" + Escape(title) + L"</text><text>" + Escape(body) + L"</text></binding></visual>";
    xml += L"<audio silent=\"true\"/>"; // astraeus plays its own sound
    if (!buttons.empty()) {
        xml += L"<actions>";
        for (const Button& b : buttons) xml += L"<action content=\"" + Escape(b.label) + L"\" activationType=\"protocol\" arguments=\"" + Escape(Link(b.action)) + L"\"/>";
        xml += L"</actions>";
    }
    xml += L"</toast>";
    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        doc.LoadXml(xml);
        winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(kAppId).Show(winrt::Windows::UI::Notifications::ToastNotification(doc));
        return true;
    } catch (...) { // notifications off for us, or the shortcut that carries our id isn't there yet: the tray's own notice instead
        return false;
    }
}

void Register(const std::wstring& exe) {
    std::wstring key = std::wstring(L"Software\\Classes\\") + kScheme, cmd = L"\"" + exe + L"\" \"%1\"";
    wchar_t now[MAX_PATH * 2] = {};
    DWORD size = sizeof(now);
    if (RegGetValueW(HKEY_CURRENT_USER, (key + L"\\shell\\open\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, now, &size) == ERROR_SUCCESS && cmd == now) return; // already right: nothing written
    const wchar_t name[] = L"URL:Astraeus";
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, REG_SZ, name, sizeof(name));
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"URL Protocol", REG_SZ, L"", sizeof(wchar_t));
    RegSetKeyValueW(HKEY_CURRENT_USER, (key + L"\\shell\\open\\command").c_str(), nullptr, REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
}

std::wstring Action(const wchar_t* commandLine) {
    std::wstring line = commandLine ? commandLine : L"", head = std::wstring(kScheme) + L":";
    size_t at = line.find(head);
    if (at == std::wstring::npos) return {};
    std::wstring action;
    for (size_t i = at + head.size(); i < line.size() && (iswalnum(line[i]) || line[i] == L'-'); ++i) action += line[i]; // only a plain word: a link is never more
    return action;
}

}
