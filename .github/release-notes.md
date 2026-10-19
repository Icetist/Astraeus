## Astraeus 2.0.1

A lighter Astraeus. It looks and works the same, it just asks much less of your PC.

### Performance
- **The pill's visualizer costs a fraction of what it did.** While a song plays, each frame now redraws only the little bars, not the whole pill with its cover and title. On the Glass look the blur outline is only re-sent when the pill's shape actually changes.
- **The bars move at a calm 15 frames a second**, as they do in HaloBar. The rest of Astraeus' motion keeps its full speed.
- **Nothing is drawn while your PC is locked or the screen is off.** The visualizer picks up again when you're back.
- **Memory is handed back to Windows** a few seconds after the pill settles.

With music playing on the Glass look, Astraeus now uses noticeably less CPU, about a third of the GPU time, and about a third of the working memory it did in 2.0.0.
