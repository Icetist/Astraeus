## What's new in 1.0.3

### Bluetooth
- **More reliable connect and disconnect for headphones and speakers.** Right after pairing, Windows' audio driver isn't ready yet and quietly ignored the first request, so pressing disconnect did nothing. Astraeus now re-sends the request until the device actually switches, instead of giving up after one try.
- The connecting spinner now waits up to 15 seconds, giving slower devices time to finish.
- Pressing connect or disconnect again while one is still in progress no longer cancels the newer spinner.

**Known issue:** some earbuds can still ignore a connect or disconnect that comes within a few seconds of the previous one. Wait a moment and press again.

### Removed
- The **Recordings** tile ("Bezel shown / Bezel hidden") in Tools. It didn't work reliably, so it's gone. Astraeus now always hides when a game or video goes fullscreen.
