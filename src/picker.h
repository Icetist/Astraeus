#pragma once
#include <windows.h>

namespace sys {

// Screen color picker: a loupe follows the cursor, a click copies the color's hex, Esc or right click cancels.
// Exists only while picking: its window, timer and mouse/keyboard hooks are made on Start and gone the moment it ends.
namespace picker {

void Start(HWND notify, UINT msg); // msg arrives with wParam = 0x00RRGGBB when picked, lParam = 1; lParam = 0 when cancelled
bool Active();

}

}
