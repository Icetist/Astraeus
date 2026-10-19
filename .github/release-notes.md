## Astraeus 2.0.2

### Fixes
- **Astraeus opens from the top edge again with a maximized terminal or browser in front.** Apps that draw their own title bar, like Windows Terminal or a Firefox-based browser, looked like fullscreen games when maximized, and Astraeus hid for them.
- **Astraeus never stays hidden after a game or video closes.** It used to wait for Windows to say the fullscreen app was gone, and that message doesn't always come. It now checks for itself every second.
