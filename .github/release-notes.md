## What's new in 1.2.0

The biggest Astraeus update yet. It can live on any edge of any screen, it has a proper settings page, a glass look, shared audio, a fullscreen player and a lot more.

### Shared audio
- **Play the same audio on several devices at once.** Open the Bluetooth menu, switch to **Shared audio** and pick any extra outputs: Bluetooth headphones, wired headphones, speakers, all at the same time and in sync. Each device gets its own delay and volume so you can line them up by ear. [so that I can listen to songs w my future gf 💔💔]
- It only runs while sharing is on, stops by itself after a few seconds of silence, and hides the low-quality hands-free versions of Bluetooth headsets.

### Fullscreen player
- **Click the album cover on the Media page** to fill the screen with what's playing: the cover big on the left, the title, artist, wavy seek bar and controls on the right, over the album art and a glow that moves with the music.
- The controls and cursor fade away when the mouse rests. Click the cover again or press **Esc** to leave. **Space** plays and pauses, the arrow keys skip.
- It follows your appearance: dark, light, or glass over a live blur of your desktop. It only exists while it's open.

### Settings
- **A new Settings page**, on the Tools page, built from small pictures of the things they change instead of rows of options.
- **Accent color.** Keep the colors from your wallpaper, or drag around the color ring to recolor everything live, pick a preset, or type or paste any HEX code. Colors that are too dark to read are brightened automatically.
- **Position.** Tap the top, left or right edge of the little monitor. On the sides Astraeus becomes a tall sidebar with everything rearranged to fit.
- **Screens.** Your monitors are shown as they're actually arranged. Tap one to keep Astraeus there, or **All** to have it open on whichever screen your mouse is on.
- **Notifications as pills.** Windows notifications can drop out of the edge as a small pill.

### Look
- **Glass appearance.** A third look next to dark and light: the panel is a live frosted blur of whatever is behind it, with a blur slider in Settings.
- **The island.** Over a maximized window, hovering the edge shows a small pill with what's playing and a tiny visualizer instead of dropping the whole panel over your work. Click it to open the panel. Works on the top, left and right edges.
- **Black-and-white album covers** now get a white or black accent instead of an invented color.
- **A clock on the weather card**, hours over minutes, in the space next to the temperature.
- **A clearer tray icon** that no longer shrinks to a speck.

### Media
- **Seek from the Home page** by dragging the progress line under the player.
- **Loop and restart for any player.** When an app has no repeat of its own, like YouTube in Firefox-based browsers, loop works by jumping back to the start just before the end. Only while loop is on; apps with their own repeat, like Spotify, keep using theirs. Previous restarts the track when the app has no previous.
- **Video thumbnails fit properly.** Wide artwork is shown whole over a blurred backdrop instead of being cropped. Square covers look exactly as before.
- **Seeking in Firefox-based browsers works reliably**, and when an app doesn't report a length, the seek bar no longer pretends.
- **Song changes show up right away**, art and title included, instead of sometimes lingering on the previous song.
- Bigger title and artist, and tighter spacing around the seek bar.

### Wi-Fi
- **Network picker.** Right-click the Wi-Fi tile, or press **Networks** on the Network page, to see nearby networks with their signal. Connect to saved or open networks in one click; new secured networks open Windows' own Wi-Fi flyout for the password.

### Safer updates
- Each release publishes the SHA-256 of `Astraeus.exe`, and the auto-updater refuses any download that doesn't match.

### Lighter
- Brushing the edge without opening the panel no longer leaves graphics memory allocated.
- When a fullscreen game or video takes over while the panel is open, everything shuts down and the GPU is handed back immediately.
- While closed, stats are sampled every 5 seconds instead of every second (about 40% less background CPU time).
- On 120 Hz and faster screens, idle animations are capped at about 30 fps.
- Glass, shared audio, the fullscreen player and the notification watcher are only loaded while you use them. Dark and light never touch any of it.

### Removed
- The recordings bezel toggle, which didn't work reliably.

**Known issue:** in Firefox-based browsers, Next only works when the website itself offers a next video.
