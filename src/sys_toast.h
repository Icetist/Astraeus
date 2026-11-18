#pragma once
// Windows' own notifications, as apps from the store show them: Astraeus' name and icon, and buttons that act.
// A button opens an astraeus: link; windows starts us with it, and that start hands the word to the copy already running.
#include <string>
#include <vector>

namespace toast {

constexpr wchar_t kAppId[] = L"Icetist.Astraeus"; // windows ties notifications to this, through the start menu shortcut that carries it
constexpr wchar_t kScheme[] = L"astraeus"; // the links the buttons open

struct Button {
    std::wstring label, action; // the action becomes astraeus:<action>
};

bool Show(const std::wstring& title, const std::wstring& body, const std::vector<Button>& buttons = {}, bool stay = false); // stay: until it's answered; false when windows won't take it
void Register(const std::wstring& exe); // the astraeus: links open this exe; kept pointing at it
std::wstring Action(const wchar_t* commandLine); // what an astraeus: link asked for, empty if it isn't one

}
