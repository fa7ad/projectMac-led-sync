# OSC wire format this project consumes (as of projectMac's SceneStream v2)

Snapshotted here so this project has a concrete, local contract to build against without
depending on another project's filesystem/repo. If it drifts from whatever a broadcaster
actually sends, trust the broadcaster's own docs over this file and update this copy.

All addresses below are shown with the default `/projectmac` prefix this project's OSC
listener should treat as configurable (see `PLAN.md`'s "OSC namespace" section) — override it
to match whatever a given broadcaster actually uses.

```
/projectmac/tempo/bpm         f        — audio energy-onset BPM estimate
/projectmac/scene/brightness  f        — Rec. 709 luma of the sampled color, 0.0-1.0
/projectmac/scene/vibrant     f f f    — saturation/population-weighted dominant color swatch
                                         (h/s/v each 0.0-1.0 — hue as a fraction of the
                                         circle, not degrees)
/projectmac/scene/muted       f f f    — second, larger/less-saturated surviving color
                                         cluster — an accent+base pair, same idea as
                                         Android Palette's Vibrant/Muted swatches
/projectmac/scene/average     f f f    — flat-average color (HSV), smoother/less-flickery
                                         than the two swatches above
/projectmac/preset/changed    (no args, a bang) — fired on every preset/scene switch
/projectmac/preset/name       s        — the new preset's name, sent alongside the bang
/projectmac/visual/bpm        f        — a *visual* onset detector's rate estimate (derived
                                         from on-screen motion/color-change, not audio) —
                                         same units as tempo/bpm, meant to be swappable
/projectmac/visual/onset      (no args, a bang) — fired the instant a visual onset is
                                         detected — an additional hard-trigger signal
                                         alongside the continuous visual/bpm above
/projectmac/tempo/phase       f        — 0.0-1.0 position within the current beat interval,
                                         audio-side — lets a consumer quantize an action to
                                         land exactly on the beat instead of "at roughly
                                         the right rate"
/projectmac/visual/phase      f        — same, visual-onset-side
/projectmac/audio/bass        f        — FFT band energy, 0.0-1.0
/projectmac/audio/mid         f
/projectmac/audio/treble      f
```

Notes relevant to this project's mapping layer:
- No raw RGB anywhere in this format — HSV only, since that's what real lighting
  protocols/fixtures want directly.
- `scene/brightness` (luma) is not the same thing as HSV's `v` (`max(r,g,b)`) — it's a
  perceptually-weighted brightness, worth treating as a distinct signal, not a duplicate.
- Per this project's own design (see `PLAN.md`): for this lamp specifically, `visual/bpm`
  should take priority over `tempo/bpm` for anything rate-driven, since it can track an
  on-screen strobe that has nothing to do with the music's actual tempo — that's a decision
  this project's mapping layer makes, not something the broadcaster decides.
