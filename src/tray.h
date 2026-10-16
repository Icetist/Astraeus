#pragma once
#include <windows.h>
#include <string>

#include "theme.h"

namespace tray {

enum Cmd : UINT { kOpen = 100, kConfig, kStartup, kRestart, kExit, kUpdate }; // sent to owner as WM_COMMAND
enum class Upd { Idle, Checking, Downloading, Current, Failed, Installed }; // what the update row says

void Show(HWND owner, const Theme& t, bool startup); // themed menu at the cursor, built on demand, freed on close
void Ask(HWND owner, const Theme& t); // themed start-with-windows prompt, yes posts kStartup
void SetUpdate(Upd state, const std::wstring& tag = L""); // the update row follows the check, live if the menu is open

}
