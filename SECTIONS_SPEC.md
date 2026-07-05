# SPEC: SECTIONS autoslice mode (Phase 3 re-scope, Task 1)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `a46e2ab`.
**Binding sources:** `Downloads\PHASE3_RESCOPE.md` (approved design) executed by
`Downloads\PHASE3_TASK_SECTIONS.md`. Where this spec conflicts with those, they win.
**Authored:** 2026-07-04 (Fable planner). Symbols/lines verified at HEAD.
**Sequencing (Joe):** Part 1 BARS → commit → Joe FL-checks. Part 2 NOVELTY →
**EAR GATE (full stop)**. Part 3 dropdown only after gate approval.

## Ground rules (inherited + task)
- No regressions: face A–D, slice/window system, tap-to-cue point-cue, stem
  engine, undo semantics. pluginval-5 stays green per commit.
- Detection on the worker thread only; `processBlock` gains zero reads.
- One slicing action = ONE CueSnap (`pushUndo()` once at click, then the action
  — pattern at PluginEditor.cpp:571-575). Re-run replaces the layout.
- Pure math → `Source/EngineMath.h` (JUCE-free) + doctest, test-first.
- No new APVTS params, deps, or Theme tokens. Menu additions use the existing
  styled `sliceMenu` PopupMenu until Part 3 builds the split-chip.

## Code ground truth (verified)
- Tempo: `getEffectiveSourceBpm()` (override else detected, h:166-171);
  `samplesPerBeat()` (cpp:599-605); grid is **0-anchored** (`nearestGridLine`,
  cpp:620-626; bar = spb*4, assume 4/4, cpp:613).
- Precedent slicer: `sliceBeats(beatsPerSlice)` (cpp:583-596) — bpm-gated
  (`bpm<=1` → no-op), 16 cues at `i*beats*spb` clamped `len-1`, `applySlices`.
  Its clamp PILES overflow pads at len-1 — BARS must not copy that.
- `applySlices(s, len)` (cpp:539-548): `cues[i] = i<s.size() ? s[i] : even16`;
  all `cueEnds=-1`. A full 16-vector passes through verbatim — **-1 entries
  yield unassigned pads** (cue=-1 = unassigned everywhere). `++uiDirty`.
- Menu dispatch: PluginEditor.cpp:567-583. **Free ids: 45-48** (taken: 1,2,
  10-13,20-22,30-32,40-43,50-52,60).
- P1 feature cache (for Part 2): `gent::FrameFeatures {band[4], centroidHz,
  zcr, chroma[12]}` per hop-512 frame (EngineMath.h:440-446), accessor
  `getFeatureFrames(out, rate)` copy-under-`infoLock` (cpp:656-660).
  Report-file precedent: `doClassifyJob` writes to `Documents\GentSampler`.

## PART 1 — BARS (deterministic; build first, own commit)

### Pure core (EngineMath.h + tests, test-first)
```
struct BarSections { std::array<int,16> cue; int sectionCount; };
BarSections barSectionSlices (int len, double samplesPerBar, int bars);
```
- Section length `L = bars * samplesPerBar`. `sectionCount = ceil(len / L)`
  (≥1 for len>0). Pad i < min(sectionCount,16): `cue[i] = (int)llround(i*L)`
  (0-anchored, grid-aligned by construction). Pad i ≥ sectionCount: `cue[i] = -1`
  (**unassigned — never clamped/piled at len-1**).
- Degenerate: `len<=0 || samplesPerBar<=0 || bars<=0` → all -1, count 0.
- Overflow is knowable: `sectionCount > 16`.
- Tests (tests/SectionTests.cpp, register in CMakeLists): exact boundaries at
  N=1/2/4/8; short source → unassigned tail (NOT len-1 pile-up — cite the
  sliceBeats contrast); long source → 16 filled from start, count>16;
  degenerates; boundary i*L never exceeds len-1 for assigned pads.

### Processor (PluginProcessor.{h,cpp})
`void sliceSections (int bars)` mirroring `sliceBeats`: `getSource()` +
`getEffectiveSourceBpm()` gate (`bpm<=1` → no-op, parity with sliceBeats);
`spb*4` bar length; call the pure core; build the 16-vector verbatim (including
-1s); `applySlices(s, len)`. On overflow (`count>16`): `DBG` one line
("SECTIONS: N sections at B bars, first 16 laid") — visible-UX note is Part 3.
**Anchor decision (assessed per task):** from the START (sample 0) — matches the
0-anchored grid and every existing mode; playhead-anchored re-slicing would make
re-run/undo non-reproducible. Revisit at Part 3 if Joe wants a "from here" verb.

### Editor (PluginEditor.cpp sliceMenu)
New submenu after "Even grid": `SECTIONS (flip)` → ids 45-48 = "Every 1/2/4/8
bars". Handler: `p.pushUndo(); p.sliceSections(N); return;` (one CueSnap).
Default for Part 3's chip = 4 bars; menu has no default state to persist yet.

### Acceptance (Part 1)
- [x] Track slices into clean N-bar, grid-aligned sections; changing N re-slices.
- [x] Short source → trailing pads unassigned (silent), no len-1 pile-up.
- [x] Long source → 16 sections from start, overflow DBG-noted, no crash.
- [x] Each section triggers as its own pad (existing slice-mode auto-end).
- [x] One undo restores the entire prior layout; redo re-applies.
- [x] gate.sh green (build/ctest/pluginval-5). Joe FL-validates before Part 2.

**Part 1 record:** committed `6253f65` (doctest 86→94). Joe FL-validated
2026-07-04 ("basic and seems fine") — Part 2 unblocked.

### AMENDMENT P2-A (2026-07-04, planner): dev-menu ids + report fields
- Dev menu uses ids **61-63 = report @ few/medium/many** and **64-66 = APPLY @
  few/medium/many** (submenu "Sections novelty (dev)"), superseding the single
  "61 report / 62 apply" allocation below — the ear gate needs Joe to try all
  three sensitivities without a settings surface, which Part 3 owns.
- The pure change curve exposes per-part distances for the report's "which
  features moved": per frame `{combined, bandDist, chromaDist}`; smoothing and
  peak-picking operate on `combined` only.

## PART 2 — NOVELTY (spectral change) + EAR GATE

### Pure core (EngineMath.h + tests, test-first)
`std::vector<int> noveltyBoundaries (frames, frameRate, samplesPerBar,
sensitivity)` — plus small testable stages (curve, smooth, pick) rather than one
monolith; exact signatures at implementer's discretion, stages individually
tested:
1. **Change curve:** per frame f>0, cosine distance between consecutive feature
   vectors: concat of L2-normalized `band[4]` and L2-normalized `chroma[12]`
   (silent/zero frames → distance 0). No new FFT — cached frames only.
2. **Smooth:** centered moving average over ~0.75 bar of frames
   (`w = max(3, round(0.75 * framesPerBar))`).
3. **Peak-pick:** local maxima above adaptive threshold `mean + k*std` of the
   smoothed curve; k by sensitivity {few: 1.5, medium: 1.0, many: 0.6}
   (**tunable table — the ONLY ear-gate tuning surface, one struct like
   ClassifierThresholds**). Enforce min section length ≥ 1 bar (drop the weaker
   peak). Snap each boundary to the nearest beat (assessed: yes, snap —
   flipping wants tight starts; snapping stays in the pure layer:
   `round(pos/spb)*spb`).
4. Boundary 0 is always a section start. >16 sections → first 16, overflow
   reported.
- Tests: synthetic frame sequences with a known timbre step → boundary lands on
  the step (±1 frame pre-snap); flat input → no boundaries; two steps closer
  than 1 bar → one boundary; sensitivity ordering (few ⊆ medium ⊆ many);
  degenerate (empty frames, zero rate/spb).

### Wiring (worker thread, report only — NO final UX before the gate)
- `requestSectionReport(sensitivity)` → worker job (mirror `doClassifyJob`):
  copy frames via `getFeatureFrames`, run the pure chain, write
  `Documents\GentSampler\GentSampler_sections_report.txt`: per boundary —
  index, time (m:ss.mmm), bar number, smoothed change score, threshold, and
  which features moved (band vs chroma delta split); footer = sensitivity, w,
  k, section count (+overflow note).
- Dev menu item (id 61, next to the classify item): "Sections (novelty) ->
  report (dev)" — **report only, does NOT slice, does NOT pushUndo**. A second
  dev item 62 "Sections (novelty): APPLY (dev)" gated behind the same pure
  output lets Joe hear it in FL (pushUndo + apply, one CueSnap) — applying is
  allowed pre-gate as a dev item because the gate JUDGES the boundaries; final
  UX is what waits.
### EAR GATE (full stop — same discipline as C5/D6)
Joe runs 2-3 real tracks, reads the report + hears the applied sections.
Verdict recorded in **this file's Gate record** before ANY Part 3 work. Tuning
touches ONLY the sensitivity/threshold/smoothing table.

## PART 3 — SLICE split-chip dropdown (UNLOCKED 2026-07-04, gate approved)

### Mode model (processor, persisted)
Four clamped atomics + setters, mirroring `sliceGridDiv`/`setSliceGridDiv`:
- `sliceModeSel` 0..4: 0 SECTIONS-BARS (default), 1 SECTIONS-NOVELTY, 2 SMART,
  3 TRANSIENT, 4 GRID-EVEN.
- `sectionBars` ∈ {1,2,4,8} (default 4); `sectionSens` 0..2 (default 1 medium);
  `gridEvenSel` 0..3 (default 3 = every bar; 0 16-equal, 1 beat, 2 2-beats).
Persist via the three-spot pattern next to `"slG"`: `saveKit` +
`getStateInformation` write, `applyStateTree` reads (keys `slMode`/`slBars`/
`slSens`/`slEven`, defaults preserved for old projects). No APVTS params.

### Split chip (editor)
Replace the `sliceMenu` TextButton with a small `SplitChip : juce::Component`
built FROM the established primitives (no new visual language): face =
`Theme::paintChip` (hover/down per zone), text = `Theme::chipFont()`, caret =
the exact triangle idiom from `GentLNF::drawComboBox` (h:2338-2342), thin
divider before an ~18px caret zone. Main-zone click → `onRun`; caret zone →
`onMenu` (the existing popup). `preferredWidth()` from font measurement
(HeroViewSeg precedent, h:2623); layoutContent uses it. Label synced by the
15 Hz timer (qualityBox precedent): `SLICE · SECTIONS` / `SLICE · SMART` /
`SLICE · TRANS` / `SLICE · GRID`.

### Run dispatch (main-zone click; ONE undoable action each)
- SECTIONS-BARS: `pushUndo(); sliceSections (sectionBars)`
- SECTIONS-NOVELTY: `pushUndo(); requestSectionApply (sectionSens)` (worker-
  deferred, CueSnap at click — the Part 2 pattern)
- SMART: `pushUndo(); autoSliceMusical()`
- TRANSIENT: `sliceBtn.onClick()` (it pushes its own undo, cpp:349)
- GRID-EVEN: call the PROCESSOR actions directly (`pushUndo()` + the same
  action the `sliceMode` combo's onChange performs for that index) — NOT via
  `ComboBox::setSelectedId`, which silently no-ops when the id is unchanged
  and would make re-run dead. Implementer mirrors the combo handler verbatim.

### Menu restructure (caret zone; same styled PopupMenu)
Top = mode section with radio ticks reflecting mode+sub-option; every item
RUNS immediately AND persists the mode:
- `SECTIONS (flip)` submenu: Bars 1/2/4/8 = existing ids 45-48 (now also set
  mode 0 + `sectionBars`); separator; `Novelty: few/medium/many` = NEW ids
  70-72 (set mode 1 + `sectionSens`, `pushUndo` + `requestSectionApply`).
- `SMART (transients + grid)` = id 1 (sets mode 2); `TRANSIENTS only` = id 2
  (sets mode 3); `GRID (even divisions)` submenu = ids 40-43 (set mode 4 +
  `gridEvenSel`, direct processor actions per the run-dispatch note).
Below, UNCHANGED: Grid-div 10-13 / Sensitivity 20-22 / Snap 30-32 param
submenus, clear items 50-52, dev section 60-66. Free ids used: 70-72.

### Acceptance (Part 3)
- [ ] Both SECTIONS sub-modes reachable from the dropdown; chip label follows
  the selected mode; main-zone click re-runs the current mode.
- [ ] Re-running ANY mode (incl. same even-grid selection twice) re-slices as
  one undoable action.
- [ ] Mode + sub-options persist across editor close/reopen AND project
  save/reload (three-spot).
- [ ] Old projects without the new keys load with defaults; no behavior change
  to any existing menu item semantics.
- [ ] gate.sh green; Joe FL-validates on a real track.

## Out of scope (all parts)
KIT changes, classifier code (parked v2), stem engine, granular, Theme.h,
processBlock, APVTS layout, anything in PHASE3_SPEC.md's regression fence.

## Verification
`bash .claude/hooks/gate.sh` (build → ctest → pluginval-5) green per commit;
new doctests included. FL validation = Joe, per part.

## Gate record (NOVELTY ear gate — append at the Part-2 STOP)
| Track | Sensitivity | Boundaries land on real changes? | Joe verdict | Notes |
|---|---|---|---|---|
| Joe's real material (FL pass) | few/medium/many available; judged live | Yes | **"works well. proceed"** — APPROVED 2026-07-04 | No kNoveltyThresh tuning requested; pre-snap min-gap watch item not reported as audible. Part 3 UNLOCKED. |
