## Astraeus 2.6.7

### New
- **Pause the timer and focus.** Click the ring to pause, and click it again to carry on from where it stopped.

### Fixed
- **Astraeus no longer asks Windows for your location over and over.** It was reading the Wi-Fi network's name every few seconds, and Windows counts that as a location read. Now it reads the name once per connection, and checks the signal only while the Network page is open.
- **The pill is reachable right after startup.** After a fresh boot, a window could end up over the top edge, so hovering there did nothing until the shortcut was used. The pill now moves back over it on its own.
- **The waveform visualizer keeps moving** for as long as music plays, instead of freezing after a quiet moment until the mouse passed over the pill.
- **The panel opens on the page you left.** A notification or the pill's bubble no longer makes it open on Tools every time afterwards.
- A custom timer longer than a day now says "24h at most" instead of "Not a time".

> [!NOTE]
> The executable is not code-signed yet, so Windows SmartScreen may show an "unknown publisher" warning the first time. Click **More info → Run anyway**.
