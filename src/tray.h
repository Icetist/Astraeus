#pragma once
#include <windows.h>

#include "theme.h"

namespace tray {

enum Cmd : UINT { kOpen = 100, kConfig, kStartup, kRestart, kExit }; // sent to owner as WM_COMMAND

void Show(HWND owner, const Theme& t, bool startup); // themed menu at the cursor, built on demand, freed on close
void Ask(HWND owner, const Theme& t); // themed start-with-windows prompt, yes posts kStartup

}
