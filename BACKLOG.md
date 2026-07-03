# BACKLOG — future specs, not scheduled

Items here need a spec before any implementation. Do not pick these up
without Joe green-lighting a spec.

## P3 retry: per-column wave-gradient "breathing" (benched 2026-07-02)
REDESIGN_C6_POLISH.md P3 was reverted after the cached 1x256 waveRamp blit
rendered the hero wave at ~half alpha (measured lum 51->27, amber lost).
Suspected but UNCONFIRMED mechanism: 1px-wide source image edge-bleed under
effectively-bilinear resampling. Any retry must (a) reproduce+confirm the
actual alpha-loss mechanism first in isolation, (b) keep the zero-per-paint-
allocation constraint, (c) pass the numeric bar: hero wave band luminance in
[43,60] with red>green by >=8, measured via the scratchpad capture rig, and
(d) compare against the mockup's per-column drawComposite. Alternatives if
the blit is unfixable: 3-segment bright-cap/dim-core vertical lines, or a
256-entry precomputed Colour ramp with per-column drawVerticalLine spans.
SEV-LOW nicety — never let it block a phase again.

## Extend undo to stem-source and grain changes (logged 2026-07-02)
CueSnap (PluginProcessor.cpp ~325, pushUndo/undo/redo) snapshots cue/end
windows only. Extend the undo snapshot to also cover per-pad stem-source
(padStemMask) and granular params (grainOn/grainPos/grainFreeze), so
Ctrl+Z after a source or grain change reverts it. Spec must decide:
snapshot granularity (per-gesture vs per-param-change), APVTS interplay
(grain params are host-automatable — undo writing params may fight host
automation), and whether the undo stack format change breaks saved-state
compat. Origin: C4 reviewer finding, Joe triaged 2026-07-02 (explicitly
NOT a Phase C6 item).
