<div align="center">

<img src="./.github/assets/logo.png" width="120" alt="Astraeus logo" />

# Astraeus

**A native bezel dashboard for Windows that drips open from the edge of your screen.**

[![Latest release](https://img.shields.io/github/v/release/Icetist/Astraeus?style=flat-square&color=9F8CFF)](https://github.com/Icetist/Astraeus/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Icetist/Astraeus/total?style=flat-square&color=9F8CFF)](https://github.com/Icetist/Astraeus/releases)
[![Build](https://img.shields.io/github/actions/workflow/status/Icetist/Astraeus/release.yml?style=flat-square&label=build)](https://github.com/Icetist/Astraeus/actions/workflows/release.yml)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D4?style=flat-square&logo=windows)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus)

[Features](#-features) •
[Screenshots](#-screenshots) •
[Performance](#-performance) •
[Installation](#-installation) •
[Configuration](#%EF%B8%8F-configuration) •
[Building](#%EF%B8%8F-building-from-source) •
[FAQ](#-faq)

<img src="./.github/assets/screenshots/home.png" width="100%" alt="Astraeus dropdown showing the media player, quick toggles and live system stats" />

</div>

Astraeus lives in a thin strip at the edge of your screen, at the top by default or as a sidebar on the left or right. Hover it and a liquid panel springs out with your music, quick toggles, system stats and everyday tools. Move away and it tucks itself back in, hands the GPU back and goes back to using almost nothing.

No Electron, no web view, no runtime to install. Just one small native executable written in C++ with Direct2D and DirectComposition.

<p align="center"><sub>If Astraeus makes your desktop a little nicer, consider leaving a ⭐. It genuinely helps.</sub></p>

## ✨ Features

- **Media player** with album art, a wavy seek bar and soft light that swells up from below with the music, bass to treble. Works with Spotify, browsers, Media Player and anything else that talks to Windows media controls. Loop and restart work even for players that don't offer them, like YouTube in Firefox-based browsers.
- **Fullscreen player.** Click the album cover to fill the screen with the cover, the song and a glow that moves with the music. Click it again or press Esc to leave, and you're back on the Media page.
- **Synced lyrics.** For songs from music apps and sites (Spotify, Apple Music, YouTube Music, Tidal, Deezer, SoundCloud and more, in their apps or in the browser), the fullscreen player shows time-synced lyrics beside the cover, even for songs Spotify has no lyrics for. Lyrics light up word by word on the beat, from three lyrics sources, the rest drift soft and out of focus, and you can click any line to jump there. Can be turned off in Settings.
- **Shared audio.** Play the same sound on several outputs at once, Bluetooth and wired, each with its own delay and volume to keep them in sync.
- **Quick toggles** for Wi-Fi, Bluetooth, microphone and output device (or power mode and battery on laptops), plus volume, mic and brightness sliders.
- **Volume mixer.** Click the speaker on the volume slider for a small mixing console: a fader and mute for every app making sound, next to the master.
- **Bluetooth control center.** Right-click the Bluetooth tile and your devices orbit your PC: connected ones close in with their battery levels, paired ones further out, new finds gliding in while it scans. Click a device to connect, disconnect or pair it, or hold to forget it.
- **Wi-Fi.** Right-click the Wi-Fi tile, or press **Networks** on the Network page: nearby networks ride the arcs of a big Wi-Fi symbol, closer the stronger they are. Join one with its password right in the panel, or hold to forget a saved one.
- **Live system stats:** CPU, GPU, memory, top processes, network throughput and latency, local and public IP, all with smooth sparklines.
- **Tools:** lock, sleep, sign out, restart and shut down (destructive actions are press-and-hold), a timer, a stopwatch with laps and focus sessions (focus and break rounds with notifications), a screen color picker that copies the hex code, a screenshot shortcut and keep-awake.
- **A keyboard shortcut.** Win+Alt+A opens the panel on the screen your mouse is on; arrow keys and 1 to 5 move between pages and Esc closes it.
- **Weather and a calendar.** Weather for your location, in Celsius or Fahrenheit. Click the weather card for a month at a glance, with holidays from around the world lit up and a greeting when you hover them, and click the weather there for the next hours and days.
- **Colors that follow you.** The palette is pulled from your wallpaper, or from the album art of whatever is playing, or from any accent you pick. Four looks: dark (the default), light, glass (a live frosted blur of whatever is behind the panel), and custom, where a color wheel lets you pick any accent and any background, with text that turns white or black to suit.
- **Settings you touch.** Pick the edge it lives on, which monitor (or all of them, with the open panel following your mouse from screen to screen), your accent color and look from small pictures of the real thing. Reduce motion calms every animation.
- **The island.** Over a maximized window, or tiled windows that fill the screen, a small pill shows what's playing instead of dropping the whole panel over your work. It can stay a moment, minutes, or always while music plays. Windows notifications can arrive as pills too.
- **Gets out of the way.** Hides itself when a game or video goes fullscreen.
- **Stays up to date.** New releases install themselves silently in the background, and only after their SHA-256 matches the one published with the release. **Check for updates** in the tray menu looks right away.
- **Lightweight.** Nothing renders while the panel is closed, the GPU is released after a few seconds and the process drops into Windows efficiency mode. Glass, shared audio and the fullscreen player only load while you use them.

## 📸 Screenshots

<table>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/media.png" alt="Media player" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/lyrics.png" alt="Fullscreen lyrics" /></td>
  </tr>
  <tr>
    <td align="center"><b>Media player</b></td>
    <td align="center"><b>Fullscreen lyrics</b></td>
  </tr>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/bluetooth.png" alt="Bluetooth control center" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/wifi.png" alt="Wi-Fi" /></td>
  </tr>
  <tr>
    <td align="center"><b>Bluetooth control center</b></td>
    <td align="center"><b>Wi-Fi</b></td>
  </tr>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/mixer.png" alt="Volume mixer" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/calendar.png" alt="Calendar" /></td>
  </tr>
  <tr>
    <td align="center"><b>Volume mixer</b></td>
    <td align="center"><b>Calendar</b></td>
  </tr>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/forecast.png" alt="Forecast" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/settings.png" alt="Settings" /></td>
  </tr>
  <tr>
    <td align="center"><b>Forecast</b></td>
    <td align="center"><b>Settings</b></td>
  </tr>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/system.png" alt="System" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/network.png" alt="Network" /></td>
  </tr>
  <tr>
    <td align="center"><b>System</b></td>
    <td align="center"><b>Network</b></td>
  </tr>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/tools.png" alt="Tools" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/pill.png" alt="The island pill" /></td>
  </tr>
  <tr>
    <td align="center"><b>Tools</b></td>
    <td align="center"><b>The island pill</b></td>
  </tr>
</table>

### Four looks

Glass (above), Dark, Light and Custom, where a color wheel picks any accent and any background.

<table>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/look_dark.png" alt="Dark look" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/look_light.png" alt="Light look" /></td>
  </tr>
  <tr>
    <td align="center"><b>Dark</b></td>
    <td align="center"><b>Light</b></td>
  </tr>
  <tr>
    <td width="50%"><img src="./.github/assets/screenshots/look_custom.png" alt="Custom look" /></td>
    <td width="50%"><img src="./.github/assets/screenshots/look_light_media.png" alt="Light look on the Media page" /></td>
  </tr>
  <tr>
    <td align="center"><b>Custom</b></td>
    <td align="center"><b>Light, with the album's colors</b></td>
  </tr>
</table>

## ⚡ Performance

Astraeus is built to cost nothing while you are not looking at it. Measured on a modest laptop
(Intel Core i5-4210U, integrated graphics, 8 GB RAM, Windows 11, 1366x768):

| | Closed | Open | After closing |
| --- | --- | --- | --- |
| **Memory (as Task Manager shows it)** | 9 MB | ~23 MB | ~2 MB |
| **CPU (share of one core)** | 0.08% | 1.7% | ~0% |

| | |
| --- | --- |
| **Executable size** | 1.9 MB, single file, fonts embedded |
| **Install size** | 1 file, no runtime, no installer |

That's light enough for 2 GB machines and entry-level Celeron and Pentium laptops: closed, it uses about a tenth of a percent of 2 GB.

How it stays this light:

- Frames are only drawn while something is actually moving. A closed panel renders nothing at all.
- Direct3D, Direct2D and the swap chain are released a few seconds after the panel closes and rebuilt on demand when it opens again. On release the driver is told to drop its buffers and freed memory goes straight back to Windows.
- Direct2D's own cache is capped and cleared after the panel closes, so a long session never grows it.
- A pill on its own keeps only a pill-sized strip of graphics memory instead of the whole panel's.
- On a PC that can't keep up, animation settles on a steady rate it can hold instead of stuttering, and tries full speed again later.
- While closed, the process runs in Windows 11 efficiency mode (EcoQoS) with low memory priority, so Windows can park it on efficiency cores and trim it first.
- Stats, Bluetooth and network workers only run while the panel is open. Bluetooth does nothing at all until you open its menu, and while closed, stats are sampled only every 5 seconds.
- The glass blur, shared audio, the fullscreen player, the audio analyser behind the visualizer and the notification watcher are built only when you use them and torn down afterwards.
- The once-a-second housekeeping timer is coalescable, so Windows can batch its wakeups with others.
- Icon fonts are subset to the glyphs that are actually used (3 MB down to 33 KB) and served straight from the executable without copying.
- A resting island pill draws its still parts once into a cached image and repaints only its four little bars, and every frame works and presents only the part of the surface the window shows.
- Lyrics are looked up once per song from three sources at once over reused connections, kept on disk for the last 300 songs so a replay needs no network, and the soft focus of each line is baked once instead of blurred every frame.
- The volume mixer reads apps only while it's open.

<sub>Private working set from Windows performance counters, after the app settles. Closed was sampled over 20 seconds, open over 10 seconds with the panel sitting on Home. The very first open of a session is higher (about 44 MB) while fonts, cover art and the forecast load; every later open settles around 23 MB.</sub>

## 📦 Installation

1. Download `Astraeus.exe` from the [latest release](https://github.com/Icetist/Astraeus/releases/latest).
2. Run it. That's it.

On first launch Astraeus asks whether it should start with Windows. Say yes and it copies itself to
`%LOCALAPPDATA%\Programs\Astraeus`, so you can delete the downloaded file whenever you like.

Updates are automatic. Astraeus checks GitHub shortly after launch and every few hours, installs new versions in the background and lets you know with a small banner the next time it restarts.

> [!NOTE]
> The executable is not code-signed yet, so Windows SmartScreen may show an "unknown publisher" warning the first time. Click **More info → Run anyway**.

## 🖱️ Usage

- **Open the panel** by hovering the middle of the edge it lives on (the top unless you moved it), with **Win+Alt+A**, or by clicking the tray icon. Over a maximized window the island pill shows first; click it to open the panel.
- **Close it** by moving the mouse away, clicking the tray icon again or dragging the panel back up.
- **Switch pages** with the rail: Home, Media, System, Network and Tools. Hover an icon to see its name. On Home, the media card, CPU, Memory and Download cards also take you to their pages. The weather card opens the calendar.
- **Fullscreen player:** click the album cover on the Media page. Click it again, press **Esc** or right-click to leave. With lyrics showing, scroll to look around, click a line to jump to it, and press **[** or **]** to nudge the lyrics earlier or later.
- **Shared audio:** open the Bluetooth menu and switch to the **Shared audio** tab.
- **Bluetooth devices:** right-click the Bluetooth tile on the Home page.
- **Volume mixer:** click the speaker icon on the volume slider, or right-click the slider.
- **Wi-Fi networks:** right-click the Wi-Fi tile on the Home page, or press **Networks** on the Network page.
- **Settings:** the **Settings** tile on the Tools page, for the look (dark, light, glass, custom), accent color, edge, screens, notification pills, the island and fullscreen lyrics.
- **Tray menu:** click the tray icon to open the dashboard; right-click it for config, start with Windows, check for updates, restart and exit.

## ⚙️ Configuration

Everything lives in `astraeus.ini` next to the executable. Open it from the tray menu (**Edit config**), save, then pick **Restart**.

| Key | Default | Description |
| --- | --- | --- |
| `PanelWidth` / `PanelHeight` | `900` / `340` | Size of the dropdown in DIPs (pixels at 100% scaling). |
| `Fillet` / `CornerRadius` | `26` / `34` | Shape of the curve where the panel meets the screen edge, and of its bottom corners. |
| `HoverOpen` | `true` | Open the panel when the mouse touches the hover zone. |
| `HoverWidth` | `240` | Width of the hover zone at the top of the screen, centered. |
| `HoverDelayMs` / `CloseDelayMs` | `70` / `450` | How long to wait before opening and closing. |
| `Hotkey` | `Win+Alt+A` | The shortcut that opens the panel, like `Ctrl+Shift+Space` or `Win+Alt+F2`; `off` for none. |
| `HideOnFullscreen` | `true` | Hide while a fullscreen app is in front. |
| `ReleaseGpuAfterSec` | `8` | Free GPU memory this many seconds after the panel closes. |
| `Theme` | `wallpaper` | `wallpaper`, or a hex color such as `#B8C4FF`. |
| `AlbumColors` | `true` | Tint the media player with colors from the album art. |
| `Visualizer` | `true` | Let the glow behind the player react to the music. |
| `Weather` / `Fahrenheit` | `true` / `false` | Show the weather card and choose its unit. |
| `AutoUpdate` | `true` | Download and install new versions automatically. |
| `PerfLog` | `false` | Write render timings to `perf.log`, handy for performance testing. |

## 🛠️ Building from source

**Requirements:** Windows 10 or 11 and Visual Studio 2022 or newer (or the Build Tools) with the **Desktop development with C++** workload.

From an **x64 Native Tools Command Prompt**, in the repository root:

```bat
mkdir build\obj
rc /nologo /fo build\obj\app.res res\app.rc
cl /nologo /std:c++20 /O2 /GL /Gw /EHsc /MT /W4 /permissive- /utf-8 /MP ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Fo:build\obj\ src\*.cpp build\obj\app.res /Fe:build\Astraeus.exe ^
   /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib dwmapi.lib ^
   d2d1.lib d3d11.lib dxgi.lib dxguid.lib dwrite.lib dcomp.lib windowscodecs.lib ^
   pdh.lib iphlpapi.lib wlanapi.lib winhttp.lib bthprops.lib powrprof.lib wbemuuid.lib ntdll.lib windowsapp.lib
```

The result is a single self-contained `build\Astraeus.exe`.

## ❓ FAQ

<details>
<summary><b>How do I uninstall it?</b></summary>

Turn off **Start with Windows** in the tray menu, exit Astraeus, then delete `%LOCALAPPDATA%\Programs\Astraeus`. Optionally remove the `HKCU\Software\Astraeus` registry key, which holds your settings.

</details>

<details>
<summary><b>Does it replace Rainmeter?</b></summary>

For a lot of people, yes. Astraeus covers the usual Rainmeter desktop widgets (media, stats, network, weather, quick toggles) in a single native app with no skins to install, and it is designed to sit at near-zero cost while idle.

</details>

<details>
<summary><b>Why did a keyboard or mouse reconnect right after I disconnected it?</b></summary>

Many keyboards, mice and controllers reconnect on their own the moment you touch them. Windows Settings behaves the same way. Headphones and speakers stay disconnected until you connect them again.

</details>

<details>
<summary><b>Can I turn off automatic updates?</b></summary>

Yes. Set `AutoUpdate = false` in `astraeus.ini` and restart.

</details>

## 🙏 Credits

- [Outfit](https://github.com/Outfitio/Outfit-Fonts) by the Outfit Project Authors, licensed under the [SIL Open Font License 1.1](./LICENSES/Outfit-OFL.txt).
- [Material Symbols](https://github.com/google/material-design-icons) by Google, licensed under the [Apache License 2.0](./LICENSES/MaterialSymbols-Apache-2.0.txt).
- Synced lyrics from [LRCLIB](https://lrclib.net), NetEase Cloud Music and Kugou.

### 🧪 The testers

**[Abrar Awwal](https://www.instagram.com/abrarr.png)** and **[ATK](https://github.com/atkftw)** ran every alpha and beta build before anyone else did. They hit the crashes, found the glitches and broke what could break, so that what reaches you doesn't.
