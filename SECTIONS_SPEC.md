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

## PART 3 — SLICE dropdown (LOCKED until gate approval)
Not specced in detail yet (deliberately). Shape per task: split-chip + mode
menu, SECTIONS (Bars 1/2/4/8 · Novelty sensitivity) + existing SMART/TRANSIENT/
GRID; selected mode+sub-option persist (session; plugin state if trivial); chip
label `SLICE · SECTIONS`. Specced after the gate.

## Out of scope (all parts)
KIT changes, classifier code (parked v2), stem engine, granular, Theme.h,
processBlock, APVTS layout, anything in PHASE3_SPEC.md's regression fence.

## Verification
`bash .claude/hooks/gate.sh` (build → ctest → pluginval-5) green per commit;
new doctests included. FL validation = Joe, per part.

## Gate record (NOVELTY ear gate — append at the Part-2 STOP)
| Track | Sensitivity | Boundaries land on real changes? | Joe verdict | Notes |
|---|---|---|---|---|
| | | | | |
