#pragma once
#include <windows.h>
#include <string>

// astraeus' settings as one .ini file: the panel's choices from the registry plus astraeus.ini itself
namespace backup {

bool Export(HWND owner, const std::wstring& iniPath); // asks where with a save dialog; false if cancelled or not written
bool Import(HWND owner, const std::wstring& iniPath); // asks which file; true once it's applied and astraeus should restart
void Reset(); // every panel setting back to its default; the caller restarts
bool ExportTo(const std::wstring& file, const std::wstring& iniPath, const wchar_t* regKey); // the work behind the dialogs, on any key
bool ImportFrom(const std::wstring& file, const std::wstring& iniPath, const wchar_t* regKey);
void ResetKey(const wchar_t* regKey);

}
