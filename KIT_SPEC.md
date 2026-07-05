# SPEC: KIT v1 — hit isolation + time-order layout + portable kit save

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `7fada79`.
**Binding source:** `Downloads\PHASE3_RESCOPE.md` §KIT (approved design). No
separate task file exists for KIT — this spec executes the rescope section
directly (flagged to Joe; he can amend). The parked classifier (PHASE3_SPEC.md
P2) stays parked: **no classification anywhere in v1.**
**Authored:** 2026-07-05 (Fable planner). Symbols verified at HEAD.
**Sequencing:** Part A (isolation + layout) → commit → Joe FL-check.
Part B (portable save) → commit → Joe FL-check. No gates beyond FL checks.

## Ground rules (inherited)
- No regressions (face A–D, slice system, SECTIONS, tap-to-cue, undo).
  pluginval-5 green per commit. Worker thread only; `processBlock` untouched.
- One slicing action = ONE CueSnap (pushUndo at click; worker-deferred OK).
- Pure math → EngineMath.h + doctest test-first. No new deps EXCEPT Part B may
  use JUCE's built-in FlacAudioFormat + ZipFile (already linked via
  juce_audio_formats/juce_core — not a new dependency).
- **NEVER terminate a user application** (CLAUDE.md landmine 2026-07-04). The
  deploy lock while FL holds the plugin is a deliberate gate: report and stop.

## Code ground truth (verified)
- `Analyzer::analyze` returns BOTH: `slices` = top-16 transients by STRENGTH
  with an **80 ms min-gap** (Analysis.h:198-241) — drops/merges hits, MUST NOT
  feed KIT — and `onsets` = ALL peaks `(samplePos, strength 0..1)`
  (Analysis.h:24,223-233), "sensitivity filters it later".
- Onset storage: `transientOnsets` under `infoLock` (h:481 vicinity); accessor
  `getOnsetPositions()` (cpp:644-652) returns positions only — Part A needs a
  strengths-preserving copy accessor (mirror it).
- Deferral precedent when analysis hasn't run: `sliceTransients` (cpp:557-573)
  sets `analysisKeepCues=false; wantAnalysis=true; notify()` and the analysis
  job applies when done (cpp:1071-1078). KIT mirrors with a `kitPending` flag.
- Split-chip mode model (Part 3): `sliceModeSel` 0-4 (h:157-180), menu ids
  used: 1,2,10-13,20-22,30-32,40-43,45-48,50-52,60-66,70-72. **Free: 80+.**
- `.gentkit` today = binary'd XML state referencing `path` (saveKit
  cpp:2792-2822; loadKit cpp:2573-2582 → applyStateTree → loadFile(path)) —
  **NOT provenance-independent**: kit breaks if the source moves. Stems are
  NOT saved (re-separated or absent on load).
- FLAC: juce_audio_formats registers FlacAudioFormat (writer supported);
  container: juce::ZipFile (read) + juce::ZipFile::Builder (write). Both
  already linked.

## PART A — hit isolation + time-order layout

### Pure core (EngineMath.h, test-first — tests/KitTests.cpp)
```
struct KitThresholds { float sFew=0.45f, sMedium=0.25f, sMany=0.12f;
                       float minSpacingSec=0.03f; };
constexpr KitThresholds kKitThresh {};
std::vector<int> kitHits (const std::vector<std::pair<int,float>>& onsets,
                          double sampleRate, int sensitivity /*0..2*/);
```
- Filter: keep onsets with `strength >= s(sensitivity)` (table above — the
  ONLY tuning surface, ClassifierThresholds discipline).
- **Completeness rule:** result in TIME order. Min-spacing `minSpacingSec`
  (30 ms — anti-double-trigger only, deliberately ≪ the 80 ms gap that makes
  `slices` merge dense-break hits): when two kept onsets are closer than the
  spacing, drop the LATER one (the earlier is the hit's true attack) — never
  strength-based dropping, that's what loses hits.
- Assume onsets ascending (they are, by construction); tolerate empty input
  (→ empty), zero sampleRate (→ empty). Do NOT cap at 16 here — the caller
  lays the first 16 and reports overflow (`sectionCount` precedent).
- Tests: dense pair at 50 ms survives as TWO hits at every sensitivity that
  passes both strengths (the completeness pin — cite the 80 ms contrast);
  double-trigger at 15 ms collapses to the EARLIER; sensitivity tiers nest
  (few ⊆ medium ⊆ many); time-order preserved; degenerates. Table-relative
  values only.

### Processor
- Accessor `getOnsetPeaks (std::vector<std::pair<int,float>>&) const` —
  copy-under-`infoLock`, mirrors getOnsetPositions.
- `void sliceKit (int sensitivity)` (message thread, undo pushed by caller):
  guards like sliceTransients; if onsets empty → set `kitPending=true`,
  `kitSensPending=sensitivity`, `analysisKeepCues=false`, `wantAnalysis=true`,
  `notify()`, return. Else compute `gent::kitHits`, lay first 16 in time order
  as cues (pad i = hit i), trailing pads -1, `cueEnds` all -1 (auto — each hit
  gates at the next hit via slice mode, the existing behavior), DBG overflow
  ("KIT: N hits, first 16 laid"). `applySlices` verbatim-16 (SECTIONS
  precedent). `padStemMask` untouched.
- Analysis-completion hook (cpp:1071-1078 block): after the existing
  `analysisThenSlice` branch, `if (kitPending.exchange(false)) sliceKit
  (kitSensPending)` — now onsets exist, runs synchronously on the worker
  (applySlices is thread-safe atomics, same as res.slices path).

### Editor (split-chip integration)
- Mode 5 = KIT: `setSliceModeSel` clamp 0..5; label case 5 → "SLICE · KIT";
  run-dispatch case 5 → `p.pushUndo(); p.sliceKit (p.getKitSens());`.
- Persisted sub-option `kitSens` 0..2 (default 1) — three-spot key "slKSens",
  clamped setter, same pattern as sectionSens.
- Menu: after the SECTIONS submenu, submenu **"KIT (every hit)"**: ids
  80/81/82 = "few / medium / many hits" (radio ticks mode==5 && kitSens==N);
  dispatch sets mode 5 + kitSens, pushUndo, sliceKit. Everything else
  byte-identical.

### Acceptance (Part A)
- [ ] Drum break: every audible hit gets its own pad in time order; two close
  hits = two pads (dense-break completeness); no hit double-triggered as two.
- [ ] Works on whatever is loaded (full mix or a separated stem — no special
  casing; stems path is just "that's the loaded source").
- [ ] >16 hits → first 16 laid, DBG overflow, no crash. Unanalyzed source →
  kit lays automatically when analysis completes (one CueSnap from the click).
- [ ] One undo restores the prior layout. Mode+sensitivity persist.
- [ ] gate.sh green. Joe FL-checks a real break before Part B.

## PART B — provenance-independent kit save (.gentkit v2)

### Design (planner decision — Joe may veto sizes)
Kit = **pad layout + audio**, loadable with the source file gone. New format:
- `.gentkit` v2 = a ZIP (juce::ZipFile::Builder) containing:
  - `kit.xml` — exactly today's state XML, plus `kitVer="2"`; `path` kept only
    as provenance INFO (never required to load).
  - `source.flac` — the loaded source buffer, FLAC-encoded (lossless, ~50%).
  - `stem0..5.flac` — iff stems exist at save time (preserves the pad
    stem-mask behavior on load; sizes are what they are — flagged to Joe).
- Loader: sniff the file — ZIP magic (`PK`) → v2 path: read kit.xml, decode
  source.flac into the SourceSample (no disk source needed; `filePath` set to
  the kit's own path for display), restore stems if present, then the normal
  applyStateTree EXCEPT its loadFile(path) branch (v2 supplies audio
  directly). Non-ZIP → legacy v1 path unchanged (old kits keep loading).
- Save path: saveKit gains the container write; encode on the WORKER (FLAC of
  minutes of audio is not message-thread work): `requestKitSave(file)` →
  `wantKitSave` job, mirror of the section jobs; completion revealed via
  `MessageManager::callAsync` (doClassifyJob idiom).
- Out of scope: kit browser, samples-as-individual-files export (EXPORT menu
  already does that), any format migration of old kits.

### Acceptance (Part B)
- [ ] Save kit → delete/rename the source wav → load kit: pads, windows,
  masks, audio all intact (the provenance test).
- [ ] Kit saved WITH stems restores stem masks audibly; without stems loads
  clean. Old v1 kits still load (path present) with today's behavior.
- [ ] Save runs on the worker; UI stays responsive during encode.
- [ ] gate.sh green; Joe FL round-trip validates.

## Out of scope (v1)
Classification / auto-arrangement (v2, parked), pad paging for >16 hits,
kit browser UI, migrating v1 kits, stem re-separation on load.

## Verification
`bash .claude/hooks/gate.sh` green per commit + new doctests. FL checks = Joe.

## Record
| Part | Commit | Joe verdict |
|---|---|---|
| A | | |
| B | | |
