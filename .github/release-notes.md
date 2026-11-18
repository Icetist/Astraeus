## Astraeus 2.7.0

### New
- **Vinyl mode.** A new look for fullscreen: the album cover becomes a record sleeve, and the record slides out to play, spinning under a tonearm that drops on as the music starts and lifts away on pause. Click the record to play or stop, and turn it by hand to seek. Turn it on in Edit fullscreen, under Cover.
- **Astraeus' own sounds.** Timers, focus rounds and alarms ring with soft bells instead of the Windows beep.
- **Notifications that act.** Timers and alarms now use Windows' own notifications, with Start again on a timer, and Snooze and Dismiss on an alarm.
- **What's new.** After an update, a card at the top of the panel shows what changed.
- In vinyl mode: a spin speed slider, an optional needle drop sound, label details, and media controls you can bring back with the Controls switch or the C key.
- Clicking a date widget on the Widgets page opens the calendar.

### Changed
- Update messages say what actually happened: offline, GitHub not answering, or a download that didn't verify.
- The two screen keys can't be set to a key fullscreen already uses, or to each other.
- Lyrics can be switched back on from the Scene page in Edit fullscreen, even while a song has none showing.

### Fixed
- **Checking for updates works on shared connections.** Astraeus no longer relies on GitHub's API, which allows only 60 checks an hour for everyone behind one internet address, so "Couldn't reach GitHub" stops appearing when GitHub was fine.

> [!NOTE]
> The executable is not code-signed yet, so Windows SmartScreen may show an "unknown publisher" warning the first time. Click **More info → Run anyway**.
