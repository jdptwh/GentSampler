# BACKLOG — future specs, not scheduled

Items here need a spec before any implementation. Do not pick these up
without Joe green-lighting a spec.

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
