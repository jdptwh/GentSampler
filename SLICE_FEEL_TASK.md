# SPEC: Slice-editing feel — relative drag, fine mode, snap capture, arrow nudge

Standalone task at HEAD `2efc2e2`. Separate from Redesign Phase C6. Origin: Joe's hands-on FL findings — handles teleport to the cursor on mouse-down, drag resolution too coarse for fine cuts, SNAP yanks handles to grid points the user doesn't want.

**Objective** — CUE/END handle editing on both surfaces (WaveformView hero map, SliceDetailStrip) drags relative to grab point with a Shift fine mode, Alt snap bypass, threshold-captured snap, and arrow-key nudging — with zero change to collapse/OPEN semantics, undo granularity, or the audio thread.

## Ground rules

1. **Single edit path (Phase C invariant).** All new drag/snap/nudge logic lives in ONE shared implementation in `Source/PluginEditor.h` next to `applyEndHandleDrag` (line 56). Both surfaces call it. No per-surface forks of any decision logic. The reviewer FAILs the task on sight of duplicated decision trees.
2. **Message thread only.** No changes to `processBlock`, voice code, or any audio-thread path. The engine calls only existing public processor API (`setCue`, `setCueEnd`, `getCue`, `getEffectiveCueEnd`, `nearestGridLine`, `nearestTransient`, `gridStepSamples`, `snapEnabled`, `pushUndo`) — all already used from PluginEditor.h today.
3. **Snap moves out of the drag path, not out of the processor.** Editor drags currently call `p.setCue(pad, x, true)` (PluginEditor.h:708, 1197) and `applyEndHandleDrag` snaps internally (PluginEditor.h:67–68). After this task: the shared engine resolves snap (threshold + Alt) and calls `setCue(pad, s, false)`; the snap block inside `applyEndHandleDrag` is deleted; `setCue`'s internal snap branch (PluginProcessor.cpp:404–406) is **kept unchanged** for its one remaining snap=true caller, `assignPadCue` (PluginProcessor.cpp:379). No double-apply is possible because the drag/nudge paths always pass `snap=false`.
4. Every criterion below is checkable by reading the strip's CUE/END/LEN readouts (PluginEditor.h:1122–1155) or by reviewer code inspection. Joe's FL feel pass is the final arbiter of "feels right"; the numbers below are the floor.

## File plan

- `Source/PluginEditor.h` — **modify.** Add `HandleDragEngine` (small struct + free functions) beside `applyEndHandleDrag`; delete the snap block from `applyEndHandleDrag` (lines 67–68); rewrite WaveformView `mouseDown/mouseDrag/mouseUp` handle branches (641–677, 707–708, 719–731, 738) and SliceDetailStrip's (1159–1223) to drive the engine; freeze strip zoom window during a handle gesture (`timerCallback`/`rebuildPeaks`, 1246–1314); active-handle affordance in strip `drawHandle` (1098–1118); `setWantsKeyboardFocus(true)` on both surfaces.
- `Source/PluginEditor.cpp` — **modify.** Extend `GentSamplerAudioProcessorEditor::keyPressed` (line 1138) with arrow/fallback nudge handling; add editor-held nudge state (active handle enum, last-nudge timestamp).
- `Source/PluginProcessor.h` / `.cpp` — **comment-only.** One line on `setCue` noting drag paths now pass `snap=false`; no functional change.
- `CLAUDE.md` — **modify.** "Current state" update on completion.
- `SLICE_FEEL_TASK.md` — **create.** This spec, repo root.

No other files. No new dependencies.

## Tasks

### F1 — Shared drag engine: relative drag + lazy undo + gesture-frozen strip zoom (WORK)

Engine state per gesture: `{ pad, handle(cue|end|grain), anchorSample, lastX, double accumSamples, bool undoPushed }`.

- mouseDown on a handle (existing hit zones unchanged: ±5 px map, ±6 px strip): record `anchorSample` = `getCue(pad)` for CUE / `getEffectiveCueEnd(pad)` for END, `lastX = e.x`, `accumSamples = 0`. **No `setCue`/`setCueEnd` call and no `pushUndo` at mouseDown.**
- mouseDrag per event: `accumSamples += (e.x − lastX) × sppNow × rate; lastX = e.x; proposed = anchorSample + llround(accumSamples)` where `sppNow` is the surface's current samples-per-pixel and `rate` = 1.0 (F2 adds 0.1). First event where `proposed ≠ anchorSample`: `pushUndo()` once (`undoPushed` guard), then apply. Incremental integration makes the model immune to mid-drag view scroll (FOLLOW) and re-anchors cleanly on rate changes.
- END applies via `applyEndHandleDrag` (snap block removed; collapse tolerance still 8 px × surface spp, `jmax(cue+33)` floor, `cue+32 → kOpenSlice` all byte-identical). CUE applies via `setCue(pad, proposed, false)`.
- SliceDetailStrip: while a CUE/END gesture is active, `zoomLo/zoomHi` are frozen (skip the hash-triggered `rebuildPeaks` for cue/end changes); recompute on mouseUp.
- In F1, snap semantics are unchanged: when `snapEnabled`, the engine applies full-strength `nearestGridLine`/`nearestTransient` to `proposed` — F3 replaces this with the threshold.

**AC-F1.1** Press-and-release on a handle with zero mouse movement: CUE/END readouts identical before/after, and Ctrl+Z afterwards undoes the *previous* edit, not a no-op (no empty undo entry).
**AC-F1.2** Grab a handle at the edge of its hit zone (~5 px off the line), SNAP off: the readout does not change on mouse-down (no teleport-to-cursor).
**AC-F1.3** SNAP off, drag exactly N px horizontally on either surface: position changes by N × spp samples ± 1 sample (verify via LEN delta on the strip).
**AC-F1.4** One completed drag gesture = exactly one undo entry (drag, release, single Ctrl+Z restores pre-drag readouts).
**AC-F1.5** During a strip handle drag the strip's zoom window does not move (waveform background stays put); it re-zooms only on release.
**AC-F1.6** Right-click on an END handle still resets to auto (`setCueEnd(pad, −1)`) with an immediate `pushUndo` — unchanged.
**AC-F1.7** Reviewer: one engine implementation; both surfaces call it; `applyEndHandleDrag` contains no snap call; no `setCue(..., true)` remains in either surface's drag path.

### F2 — Fine mode: Shift = 0.10× (WORK)

- Shift held during a CUE/END drag sets `rate = 0.10` exactly, evaluated per mouseDrag event inside the F1 accumulator.
- Mid-drag Shift press/release re-anchors implicitly (incremental accumulation): the position discontinuity at the transition event is ≤ 1 sample.
- Fine mode composes with snap as resolved in F3 (threshold test runs on the fine-accumulated `proposed`).

**AC-F2.1** SNAP off: a ~100 px plain drag vs. a ~100 px Shift drag from the same start produce LEN deltas in a 10:1 ratio ± 20 % (readout check).
**AC-F2.2** Drag 50 px plain, press Shift without moving: readout does not jump. Continue 50 px: total displacement ≈ 50×spp + 50×spp×0.1 samples ± 2 samples.
**AC-F2.3** Release Shift mid-drag: no jump at the release event (readout continuous, ≤ 1 sample discontinuity).

### F3 — Snap capture threshold + Alt bypass (WORK)

Resolve step in the engine, replacing F1's interim full-strength snap; shared verbatim by both surfaces:

```
if (p.snapEnabled && !e.mods.isAltDown() && handle drag):
    cand = gridStepSamples() > 0 ? nearestGridLine(proposed) : nearestTransient(proposed)
    resolved = (|cand − proposed| <= 6 × sppNow) ? cand : proposed
else: resolved = proposed
```

- **Capture threshold = 6 screen px** at the active surface's current zoom (6 × sppNow samples), consistent with the Phase C px→sample tolerance pattern. Outside 6 px of a grid/transient point the handle tracks the accumulator exactly. (`nearestTransient`'s existing 50 ms internal cap, PluginProcessor.cpp:304, stays; effective transient capture = min of the two.)
- Alt held = snap fully bypassed; Alt released mid-drag re-enables on the next event (modifiers read per event — no extra state).
- `setCue`'s internal snap branch untouched (serves `assignPadCue` only). `snapCursor`/`placeStart` cursor snapping untouched.

**AC-F3.1** SNAP on, GRID = beat: dragging between two grid lines, the handle sits wherever released when > 6 px (screen) from the nearest line — readout differs from the grid-line value.
**AC-F3.2** Same setup, release within ≤ 6 px of a grid line: readout equals that grid line's value exactly.
**AC-F3.3** SNAP on + Alt held: handle lands on arbitrary positions even 1 px from a grid line; releasing Alt mid-drag restores capture behavior in the same gesture.
**AC-F3.4** Reviewer: exactly one snap-resolve implementation; `applyEndHandleDrag`, `assignPadCue`, `snapCursor`, and all auto-slice paths (`applySlices`, `sliceBeats`, `computeBlendedSlices`) verified unchanged.

### F4 — Arrow-key nudge + active-handle affordance (WORK)

- Handled in `GentSamplerAudioProcessorEditor::keyPressed` (PluginEditor.cpp:1138 — non-Ctrl branch, currently `return false`). Both surfaces get `setWantsKeyboardFocus(true)` so clicking them puts keyboard focus inside the plugin window; unhandled keys bubble to the editor's key listener, so the handler lives in one place.
- **Bindings:** Left/Right = nudge active handle; Shift+Left/Right = fine nudge; Up = arm CUE, Down = arm END. **FL fallback** (FL's typing-to-piano can steal arrows): comma `,` = nudge left, period `.` = nudge right, same Shift behavior, same handler. Precondition in FL either way: click the map or strip once first.
- **Nudge target:** editor-held state — the handle most recently grabbed on either surface for the selected pad; defaults to CUE; resets to CUE on pad-selection change.
- **Increments:** default = 1 strip-pixel = `stripSpp` samples (`(zoomHi−zoomLo)/(waveR−waveL)`); Shift = `max(1, llround(stripSpp/10))` samples. Rationale: matches drag resolution 1:1 (one arrow press = one pixel of strip drag) and self-scales because the strip auto-zooms to the slice span +15 %. No-op when the selected pad is unassigned or no source loaded.
- **Snap never applies to nudges** (nudge is the precision escape hatch). CUE via `setCue(pad, s, false)`; END via `applyEndHandleDrag` with tol = 8 × stripSpp (collapse-to-OPEN reachable by nudging, same rule as drag).
- **Undo coalescing:** `pushUndo()` only if ≥ 600 ms since the last nudge edit; a rapid burst = one undo entry.
- **Affordance:** on the strip only, the armed handle's triangle cap renders full-alpha `Theme::accent` with a 1 px accent outline ring; the other cap renders as today. Map handle rendering unchanged.

**AC-F4.1** Standalone build: click the strip, press Right ×5 → armed handle's readout advances 5 × stripSpp samples ± 1/press; Shift+Right advances by max(1, stripSpp/10).
**AC-F4.2** Up then Right moves CUE; Down then Right moves END (readouts confirm).
**AC-F4.3** Comma/period produce identical deltas to Left/Right.
**AC-F4.4** A burst of 10 nudges inside 600 ms gaps = one Ctrl+Z to fully revert.
**AC-F4.5** SNAP on: nudges land at exact ±increment positions, never pulled to grid lines.
**AC-F4.6** Armed-handle cap is visibly distinct on the strip; arming state changes with Up/Down and with grabbing either handle by mouse.
**AC-F4.7** Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y still work (keyPressed's ctrl branch untouched).
**AC-F4.8 (Joe, FL)** Arrows reach the plugin after clicking the map/strip; if FL steals them, comma/period work. Recorded in the feel-pass verdict.

### F5 — Grain marker consistency (WORK, last)

Decision (explicit, per spec authority): the strip's grain-position marker (`DragMode::grainPos`, PluginEditor.h:1177–1178, 1209–1215) **gets the relative-drag model and Shift fine mode via the same engine** — same grab feel as the handles, no teleport. It gets **no snap** (never had it) and **no arrow nudge** (it is a performance control, not a cut point). Position still clamped to [cue, end] and stored as fraction via `setGrainPosFor`.

**AC-F5.1** Grab the marker off-center within its 6 px zone: no jump at mouse-down.
**AC-F5.2** Shift during marker drag = 0.10× rate, no jump at Shift transitions.
**AC-F5.3** Reviewer: marker path reuses the F1 engine accumulator — no third drag implementation.

## UI acceptance criteria

(JUCE plugin editor — pluginval editor tests + reviewer inspection + Joe's FL pass stand in for Playwright.)

1. Core flow: load a file, slice, grab a strip END handle, make a sub-10 ms cut adjustment (Shift-drag or nudge) in ≤ 3 interactions after selecting the pad — no teleport at any step.
2. Both handles remain grabbable at the existing hit zones at the 880×592 window floor; the armed-handle affordance is visible at that size.
3. pluginval strictness 5 passes with the editor open/close cycle — no asserts, leaks, or focus-related crashes from the new `setWantsKeyboardFocus` calls.
4. Standalone and FL VST3 behave identically for F1–F3 and F5 (F4 arrows may differ per host; fallback keys must work in both).

## Design source

None. No Claude Design handoff bundle exists for this task; it is behavioral. Visual changes are limited to the F4 armed-handle cap treatment using existing `Theme` tokens.

## Verification commands

```
cmake --build build --config Release --parallel
bash .claude/hooks/gate.sh   # build + pluginval strictness 5
```

Both must pass after every task F1–F5, not only at the end.

## Out of scope — regression fence (must NOT change)

- Collapse-to-open semantics: 8 px per-surface tolerance, `setCueEnd` `cue+32 → kOpenSlice` (PluginProcessor.cpp:419–420), `jmax(cue+33)` minimum window.
- `setCue`'s start-pushed-past-end auto-clear (PluginProcessor.cpp:409–410) and its snap branch for `assignPadCue`.
- `snapCursor` / `placeStart` audition-cursor behavior (PluginEditor.h:679–687, 710–717); scrollbar, middle-drag pan, wheel zoom, double-click FULL VIEW, FOLLOW; flag-pennant selection; stem lane mute/solo hit zones; right-click END reset.
- Auto-slice / transient-slice paths: `applySlices`, `sliceTransients`, `sliceGrid`, `sliceBeats`, `computeBlendedSlices` — zero edits.
- Undo depth (64), `snapshot`/`applySnap`, Ctrl+Z/Y bindings.
- Anything in `processBlock`, the voice pool, SpinLock usage, ONNX/stem code, `build/`, CMakeLists.txt.
- The one *intended* behavior change: click-without-move on a handle no longer creates an undo entry (previously `pushUndo` fired at mouseDown). This is spec'd, not a regression.

## Tier assignment

- F1–F5: **WORK (Sonnet)**, strictly in order F1 → F2 → F3 → F4 → F5, gate between each. No BULK anywhere: the project has no test suite, so the only machine check is the compiler, which cannot catch drag-feel or snap-logic errors — ROUTING.md Rule 1 therefore sets WORK as the floor for every item.
- Reviewer (GATE): after F3 (the snap refactor — highest blast radius) and after F5 (full diff).
- Two Sonnet failures on any task → escalate to GATE with both attempts attached (ROUTING.md Rule 4).

## Risks

1. **FL swallows arrow keys** → comma/period fallback spec'd in F4; feel pass verifies both; worst case arrows are documented FL-limited and fallback is primary.
2. **Strip re-zoom feedback loop during drag** → explicitly fixed by F1's gesture-frozen zoom window; reviewer checks the freeze is gesture-scoped only.
3. **Accumulator drift vs. absolute mapping** → `double` accumulation, ≤ 1 sample rounding per gesture; AC-F1.3 catches it.
4. **Double-snap** (engine + `setCue`) → structurally prevented: drag/nudge paths pass `snap=false`; AC-F3.4 reviewer check.
5. **Focus changes ripple** (`setWantsKeyboardFocus` on two components could alter Ctrl+Z routing) → AC-F4.7 explicitly retests undo keys; pluginval editor cycle covers open/close focus safety.
6. **OPEN-slice end-handle anchor ambiguity** (anchor = `getEffectiveCueEnd` = len−1 for OPEN) → spec'd in F1; dragging an OPEN end left immediately produces a real window once past `cue+33`, same as today.

## Definition of Done

- [ ] `cmake --build build --config Release --parallel` green after each task
- [ ] pluginval strictness 5 green (gate.sh)
- [ ] All AC-F1.x … AC-F5.x checked off individually
- [ ] Reviewer PASS on the F3 refactor and the final diff (single-edit-path invariant explicitly confirmed)
- [ ] **Joe's hands-on feel pass in FL Studio** covering: no-jump grab, Shift fine ratio, Alt bypass, snap capture at 6 px, arrow/fallback nudge — his sign-off is the final gate and can bounce the task even with all machine gates green
- [ ] No files touched outside the file plan
- [ ] CLAUDE.md "Current state" updated

---

*Key file references: `Source/PluginEditor.h` (engine site: lines 47–71; map handlers 567–738; strip handlers 1159–1223; strip zoom rebuild 1284–1314), `Source/PluginEditor.cpp` (keyPressed at 1138), `Source/PluginProcessor.cpp` (setCue 398, setCueEnd 413, nearestGridLine 530, nearestTransient 293, snapCursor 540, pushUndo 335). Spec authored by planner (Fable) 2026-07-02 from Joe's FL feel findings; HEAD at authoring: 2efc2e2.*
