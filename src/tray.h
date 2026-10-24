#pragma once
#include <windows.h>
#include <string>

#include "theme.h"

namespace tray {

enum Cmd : UINT { kOpen = 100, kConfig, kStartup, kRestart, kExit, kUpdate, kSettings }; // sent to owner as WM_COMMAND
enum Outside : UINT { kWake = 109, kPlayFull = 110 }; // WM_COMMAND from other processes: a second launch, halobar's cover. fixed numbers, others rely on them
enum Backup : UINT { kExport = 120, kImport, kReset }; // from the settings sheet: dialogs and the restart run on the host
enum class Upd { Idle, Checking, Downloading, Current, Failed, Installed, Updated }; // Updated: first run after a swap // what the update row says

void Show(HWND owner, const Theme& t, bool startup); // themed menu at the cursor, built on demand, freed on close
void Ask(HWND owner, const Theme& t); // themed start-with-windows prompt, yes posts kStartup
void SetUpdate(Upd state, const std::wstring& tag = L""); // the update row follows the check, live if the menu is open

}
