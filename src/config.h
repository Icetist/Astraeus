#pragma once
#include <string>
#include <vector>

#include "gfx.h"

struct Config {
    float panW = 900; // dips, defaults mirror kDefault ini
    float panH = 340;
    float fillet = 26; // concave curve where panel meets screen top
    float corner = 34;
    bool hoverOpen = true;
    int hoverMs = 70;
    float hoverW = 240; // hot zone width at screen top while closed
    int closeMs = 450;
    bool hideFull = true;
    int gpuIdleSec = 8; // drop d2d resources after closed this long
    bool perfLog = false;

    bool wallTheme = true; // false = use themeCol as seed
    Color themeCol = Color::Rgb(0xB8C4FF);
    bool albumCol = true;
    bool viz = true;

    bool weather = true;
    bool fahr = false;
    bool autoUpdate = true; // install new github releases silently

    std::vector<std::wstring> warns; // parse problems, shown to user, never fatal

    static Config Load(const std::wstring& path);
};
