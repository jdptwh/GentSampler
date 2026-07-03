# SPEC: Phase 3 — Bench Cleanup + Semantic Slicing (SLICE modes dropdown, KIT auto-mapping)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ HEAD `728f155` (redesign A–D complete, 54 ctest cases, gate order build → ctest → pluginval)
**Binding source doc:** `C:\Users\JoeyD\Downloads\PHASE3_TASK.md` (Joe, stays in Downloads — this spec executes it; where this spec and that doc conflict, that doc wins and this spec gets amended)
**Authored:** 2026-07-03 (Fable planner). All symbols/lines below verified against HEAD by direct reading.

**Objective** — Clear the bench (standalone ORT DLLs, pluginval retry, honest undo, optional P3 retry), then ship slice-by-musical-function: cached analysis features → heuristic KICK/SNARE/HAT/PERC/TONAL/OTHER classifier (stems-assisted, Joe-ear-gated on real material) → KIT pad mapping → a SLICE split-chip mode dropdown — with no ML, no models, no regressions, everything undoable.

---

## Ground rules

1. **Do not regress:** the redesigned face (Phases A–D incl. COMPOSITE⇄STEMS hero), slice/window system, stem engine, granular/bleed/transcription, pad drag. pluginval strictness 5 stays green after every task.
2. **New UI uses established primitives only:** chip plates (Theme.h tokens), the caret idiom from `GentLNF::drawComboBox` (PluginEditor.h:2331-2342), the styled `juce::PopupMenu` already used by `sliceMenu` (PluginEditor.cpp:511-561), the text-sized-component precedent `HeroViewSeg::preferredWidth()` (PluginEditor.h:2623). No new visual language, no new Theme tokens.
3. **Classification and feature caching run on worker threads** — specifically the processor's existing private `juce::Thread` run loop that already dispatches `doAnalysisJob`/`doStemJob` via `wantAnalysis`/`wantStems` flags. Never the audio thread. No allocation, locking, or new reads added to `processBlock`.
4. **All slicing actions (including KIT remap) go through the existing undoable path:** editor calls `p.pushUndo()` ONCE at click time, then invokes the processor action — the exact pattern at PluginEditor.cpp:549-553, which already handles worker-deferred slicing correctly (snapshot precedes the eventual `applySlices` even when analysis runs first).
5. **HARD GATE after Part 2.** No Part 3 or Part 4 code exists until Joe's written approval of the classification reports is recorded in this file's gate record. Same stop discipline as the C5/D6 gates.
6. **Commit per task** (0.1, 0.2, 0.3, 0.4 if done, P1, P2, P3, P4 — each its own commit).
7. **Pure logic goes to `Source/EngineMath.h` + doctest, test-first** where designated. EngineMath.h stays JUCE-free.
8. **No new APVTS parameters, no new dependencies, no new FetchContent.**

---

## Code ground truth (implementer orientation — verified citations)

- `Analyzer::analyze` (Source/Analysis.h:29-233): computes spectral flux from 1024-pt FFT magnitudes at hop 512 (lines 49-81), BPM autocorrelation (86-115), top-16 transient slices + full onset list `(samplePos, strength 0..1)` (117-168), and a **global** (not per-frame) chroma at 4096-pt/hop-4096 for key detection (170-230). Per-frame magnitudes and per-frame chroma are currently **discarded** — only `AnalysisResult{bpm, key, slices, onsets}` (18-24) survives. ZCR is absent. Runs inside `doAnalysisJob` (PluginProcessor.cpp:926-959) on the processor's worker thread; results land under `infoLock` (938-942); rebuild trigger is `wantAnalysis` (set by `loadFile` runAnalysis=true path, cpp:293-298, and by slice actions when onsets are missing, cpp:479-482, 637-643).
- Slicing paths (PluginProcessor.cpp): `applySlices` (457-466, writes cues + end=-1 for all 16), `sliceTransients` (468-485), `sliceGrid` (487-492), `sliceBeats` (494-507), `computeBlendedSlices` (565-628, **this is SMART's core** — sensitivity/snap/minGap, caps at 16, forces pos-0 first slice), `autoSliceMusical` (630-651, SMART's entry, defers via `analysisThenSlice`). The SLICE chip is `sliceMenu` (TextButton, PluginEditor.h:2726), bottom-left hero chip row at PluginEditor.cpp:937-943 (52px wide), popup + dispatch at cpp:511-561. Undo: one `pushUndo` per slice action, pushed by the **editor** before invoking (cpp:549-559); `doAnalysisJob`'s deferred `applySlices` does NOT push (the click already did).
- Undo internals: `CueSnap { std::array<int,16> cue, end; }` (PluginProcessor.h:389); `snapshot`/`applySnap`/`pushUndo`/`undo`/`redo` (cpp:336-384); history is a 64-deep runtime vector, **never serialized** → growing CueSnap has **zero saved-state compat impact**. `clearCue` resets `padStemMask` too (cpp:416).
- Stems: `StemSet` = 6 source-domain, source-aligned buffers (PluginProcessor.h:42-54; order drums, bass, vocals, guitar, piano, other) — cues are source-domain, so **per-slice per-stem energy is a plain sample-range energy sum over `stemSet` buffers**, done on the worker thread. `renderedStems` (stretched domain) is the wrong buffer for this — do not use it. `hasStems()` = `stemSet != nullptr` under `stemLock`.
- Grain params: 7 per-pad APVTS params `pGrainOn/Size/Dens/Pos/Freeze/Spray/Pitch` (h:473-474); message-thread write precedent `apvts.getParameterAsValue(pid(pad,"grainPos")) = v` (h:259). `padStemMask` = 16 `atomic<uint8_t>`, written by `setPadStemBit`/`setPadFull` from the inspector SOURCE chips (PluginEditor.cpp:440-453 — currently **no pushUndo there**). The grain-marker drag deliberately skips pushUndo today (F5 comment, PluginEditor.h:164-176).
- Persistence three-spot: write in `saveKit` (cpp:~2153 `"slG"` neighborhood) + `getStateInformation` (cpp:~2788), read once in `applyStateTree` (cpp:~2217). Editor timer (15 Hz, cpp:612) re-syncs chip state from the processor — precedent: `qualityBox` sync cpp:1598-1600, `viewSeg` cpp:1517-1518.
- gate.sh: pluginval gate at lines 70-74 via `run_gate`; stray-process taskkill at 64; CLAUDE.md "Next up" records two transient pluginval failures that passed on immediate rerun.
- CMakeLists: ORT core-DLL copy targets **only** `GentSampler_VST3` (116-121); Standalone gets nothing (BACKLOG entry). Test target block at 138-153 — append-owned, safe to extend its source list.

---

## File plan

| File | Change |
|---|---|
| `CMakeLists.txt` | 0.1: append Standalone ORT-copy block after line 121. P1-P3: add 3 test .cpp files to `GentSamplerTests` sources (lines 142-147). Nothing else touched. |
| `.claude/hooks/gate.sh` | 0.2: pluginval gate gains retry-once (`run_gate_retry`). |
| `Source/PluginProcessor.h` | 0.3: `CueSnap` grows mask+grain arrays; undo/redo documented message-thread-only. P1: feature-cache storage + accessor. P2/P3: `requestClassifyReport()`, `sliceKit()`, `kitPending`/`wantClassify` flags. P4: `sliceRunMode`/`sliceGridBeatsSel` atomics + accessors. |
| `Source/PluginProcessor.cpp` | 0.3: `snapshot`/`applySnap` extended. P1: `doAnalysisJob` stores frame features under `infoLock`. P2: `doClassifyJob` (features→aggregates→classify→report file). P3: `sliceKit` chain + deferred-analysis hook in `doAnalysisJob`. P4: three-spot keys `"slMode"`/`"slBeats"`. |
| `Source/Analysis.h` | P1: `AnalysisResult` + frame-feature computation inside the existing FFT loop (no second FFT pass). |
| `Source/EngineMath.h` | P1: `FrameFeatures`, `aggregateSliceFeatures`. P2: `SliceClass` enum, `SliceFeatures`, `ClassifierThresholds` table, `classifySlice`. P3: `KitSlice`, `mapKitPads`. |
| `Source/PluginEditor.h` | 0.3: grain-marker drag pushUndo (revise F5 comment, h:164-176); grain-knob `onDragStart` pushes. P4: new `SliceSplitChip` component (styling precedent: `HeroViewSeg`, h:2591+). Optional: pad class glyph in `PadButton` paint. |
| `Source/PluginEditor.cpp` | 0.3: SOURCE chip clicks push undo (440-453). P2: dev "Classify slices → report" item in `sliceMenu` popup. P4: `sliceMenu` TextButton replaced by `SliceSplitChip`; layout (943) uses `preferredWidth()`; timer sync of label/mode (1590s neighborhood); menu restructure. |
| `tests/FeatureAggTests.cpp` | **NEW** — P1 aggregation tests. |
| `tests/ClassifierTests.cpp` | **NEW** — P2 classifier tests. |
| `tests/KitMapTests.cpp` | **NEW** — P3 mapping property tests. |
| `CLAUDE.md` | Current-state updates per commit; 0.3 rewrites the "Undo scope is PARTIAL" landmine — see Landmines. |
| `BACKLOG.md` | 0.1 ships → delete ORT-DLL entry; 0.3 ships → delete extend-undo entry; 0.4 done → delete P3 entry (skipped → entry stays, annotated with the new experiment results). |
| `PHASE3_SPEC.md` | **NEW** — this file, repo root; Part-2 gate record appended in place. |

Optional-task files (0.4): `Source/PluginEditor.h` hero wave paint + scratchpad capture rig (outside repo).

---

## PART 0 — Bench cleanup (small, do first, commit each)

### 0.1 Standalone ORT DLLs (BULK)

Append after CMakeLists.txt line 121 (mirror of the VST3 block):

```cmake
add_custom_command(TARGET GentSampler_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_NATIVE_DIR}/onnxruntime.dll"
        "${ORT_NATIVE_DIR}/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:GentSampler_Standalone>"
    VERBATIM)
```

**AC-0.1:**
1. Build green; `build/GentSampler_artefacts/Release/Standalone/` contains both `onnxruntime.dll` and `onnxruntime_providers_shared.dll`.
2. `git diff CMakeLists.txt` shows only the appended block; ORT pin block, VST3 flags, deploy step untouched.
3. BACKLOG.md ORT entry removed in the same commit.

### 0.2 gate.sh pluginval retry-once (WORK)

Add a `run_gate_retry` function beside `run_gate`: identical shape, but on first failure it prints a `retrying once (transient-flake, CLAUDE.md note)` line, runs `taskkill //F //IM pluginval.exe >/dev/null 2>&1 || true; sleep 2`, reruns once; second failure exits 2. Switch **only** the pluginval invocation to `run_gate_retry`; build/tests/lint gates keep single-shot `run_gate`.

**AC-0.2:**
1. `bash -n .claude/hooks/gate.sh` clean.
2. Fault injection: with `CLAUDE_PLUGINVAL_CMD` pointed at a scratchpad script that fails on first invocation and succeeds on second (marker-file trick), gate.sh prints exactly one retry line and exits 0; with an always-failing script it runs exactly twice and exits 2.
3. Full real `bash .claude/hooks/gate.sh` green end-to-end.
4. Update the gate-list header comment to note pluginval retries once.

### 0.3 Extend undo — stem-source + grain into CueSnap (WORK + R0 gate)

This resolves the BACKLOG extend-undo entry's three open questions. **Decisions (normative):**

- **D-0.3a — Snapshot contents.** `CueSnap` grows to `{ std::array<int,16> cue, end; std::array<std::uint8_t,16> mask; std::array<std::array<float,7>,16> grain; }` — grain order: on, size, dens, pos, freeze, spray, pitch (matching `pGrain*` declaration order). All 7 grain params covered ("tweak grain knobs → undo restores" is binding in PHASE3_TASK.md 0.3). ~38 KB worst-case for the 64-deep stack — negligible.
- **D-0.3b — Gesture granularity: per-gesture.** One `pushUndo` at gesture start: SOURCE chip click = one gesture; grain toggle click = one gesture; grain knob = push on `Slider::onDragStart` (NOT per value change); grain-marker drag in SliceDetailStrip = push on gesture arm, mirroring the lazy `undoPushed` first-effective-movement guard. The F5 exclusion comment is rewritten — its stated reason dissolves now that the snapshot carries grain.
- **D-0.3c — APVTS/automation interplay: undo writes grain params via `apvts.getParameterAsValue(pid(pad, ...)) = v`** (existing precedent), message thread only, **write-if-changed** (skip params whose current value already matches the snapshot — avoids storms of 112 notifications per undo). No `beginChangeGesture`/`endChangeGesture` bracketing. Consequence, accepted and documented in-code: if the host is actively automating a grain param, host automation overwrites the undo result on its next pass — identical to how any UI write behaves in every DAW; undo does not fight it. This is the decided answer to the BACKLOG's interplay question.
- **D-0.3d — State-format compat: non-issue.** The undo stack is runtime-only (never serialized — verified). No kit/session format change, no version key needed.
- `applySnap` also gets write-if-changed semantics for `mask` (plain atomic stores) and `++uiDirty` as today. `undo()`/`redo()` get a "message thread only" comment (they now touch APVTS); the only callers are the editor's undo/redo buttons and Ctrl+Z/Y — verify and state this in the R0 review.

**AC-0.3:**
1. Build + full ctest green (existing 54 cases untouched).
2. FL/standalone manual verify: change a pad's SOURCE chip → Ctrl+Z restores prior source; toggle grain on + drag two grain knobs (two gestures) → two Ctrl+Z steps restore each; drag grain marker → Ctrl+Z restores position; redo re-applies all of the above.
3. A knob drag produces exactly ONE history entry regardless of drag length.
4. Slice-action undo unchanged: SMART slice → Ctrl+Z restores prior cues AND (new) prior masks/grain — one step.
5. CLAUDE.md landmine rewrite + BACKLOG entry deletion in the same commit.
6. pluginval strictness 5 green.

### 0.4 P3 breathing-wave retry (OPTIONAL, last in Part 0, WORK)

Governed by the BACKLOG entry's own rules. **Isolation experiment first, in the scratchpad capture rig, zero production diff:** (a) reproduce the alpha loss — blit a 1×256 amber ramp image to a canvas at hero scale, measure band luminance; (b) confirm/refute the suspected mechanism by comparing a 1px-wide source against a 2px-wide source with duplicated edge columns and against low resampling quality; (c) only a **confirmed** mechanism with a rig-passing fix (luminance in [43,60], red>green ≥ 8) may be ported to the hero paint, keeping the zero-per-paint-allocation constraint, verified by the same rig + side-by-side vs the mockup. **Skip criteria (any → SKIP, keep BACKLOG entry, annotate with rig findings, zero production diff):** mechanism not confirmed in the rig session; more than 2 fix approaches attempted; numeric bar unmet; any per-paint allocation required.

**AC-0.4 (only if done):** rig numbers recorded (before/after luminance + red-green delta); hero band luminance in [43,60] with red>green ≥ 8; build/ctest/pluginval green; sbs capture archived to Desktop per the D6 convention.

---

## PART 1 — Feature caching (WORK; aggregation math BULK test-first)

### Analyzer changes (Source/Analysis.h — WORK)

Inside the **existing** flux FFT loop (reuse the magnitudes already computed, add no second FFT pass), compute per frame and store:

- **4 band energies** (sum of squared magnitudes): bins mapped to <120 Hz / 120–600 / 600–3k / >3k (bin→Hz via `b * sr / fftSize`; band edges as named constants).
- **Spectral centroid** (Hz, magnitude-weighted over all bins; 0 for silent frames).
- **ZCR**: zero-crossing count / window length over the same 1024-sample windowed region of `mono` (time-domain, same loop pass — ZCR is currently absent, this adds it).
- **12-bin chroma** per frame from the same 1024-pt magnitudes, restricted to 55–1760 Hz (same fold as the key detector). Stated limitation, acceptable: 43 Hz bin spacing is coarse below ~200 Hz; chroma here feeds tonal-vs-noisy flatness only, not key naming.

`AnalysisResult` gains `std::vector<gent::FrameFeatures> frames;` plus `int hop = 512; double frameRate;` where `gent::FrameFeatures` (EngineMath.h, plain struct) = `{ float band[4]; float centroidHz; float zcr; float chroma[12]; }` — 72 bytes/frame.

**Memory budget:** frames = n/512. A 4-min 44.1 kHz source → ~1.5 MB; 10-min → ~3.7 MB. **Hard cap AC: ≤ 8 MB** for a 10-min file (no downsampling needed at hop 512; keeping hop alignment with `onsets` makes attack-window aggregation exact).

**Thread/rebuild story:** computed inside `Analyzer::analyze` (already on the worker); stored on the processor under `infoLock` beside `transientOnsets` as `featureFrames` + `featureFrameRate`; cleared where `transientSlices` clears on new load; rebuilt whenever analysis reruns. Audio thread never touches it. Editor never copies it (worker-side consumers only).

### Pure aggregation (EngineMath.h — BULK test-first)

```cpp
struct SliceAggregates { float bandRatio[4]; float centroidHz; float zcr;
                         float decaySec; float durationSec; float chromaFlatness; };
SliceAggregates aggregateSliceFeatures (const std::vector<FrameFeatures>& frames,
                                        double frameRate, int startFrame, int endFrame);
```

Rules (normative): attack window = first `kAttackWindowSec = 0.12f` seconds of the slice (or the whole slice if shorter) — `bandRatio` (normalized to sum 1; all-zero → equal 0.25s), `centroidHz` (energy-weighted mean), `zcr` (mean) aggregate over the attack window only. `decaySec` = time from the slice's peak-total-energy frame to the first later frame with energy ≤ peak × 0.01 (−20 dB), else slice end. `durationSec` = slice span. `chromaFlatness` = geometric mean / arithmetic mean of the slice-summed 12-bin chroma ∈ (0,1] (1 = flat/noisy, →0 = peaked/tonal; empty/zero chroma → 1.0). Degenerate inputs → zeroed struct with `durationSec = 0`.

**AC-P1 (tests in `tests/FeatureAggTests.cpp`, written BEFORE the BULK implementation):**
1. Band ratios sum to 1 (±1e-4) for random non-silent frames; equal-split on silence.
2. Attack weighting: frames after 0.12 s provably excluded from bandRatio/centroid/zcr.
3. Decay cases: instant decay, no decay (returns span), mid-slice decay computed to frame precision.
4. Chroma flatness: single-bin chroma → < 0.35; uniform chroma → 1.0.
5. Degenerate ranges → zeroed struct; determinism.
6. Analyzer side (WORK): 10-min buffer keeps `frames` ≤ 8 MB; full ctest + pluginval green; `analyze`'s existing outputs behavior-identical (additive lines only — reviewer diff check).

---

## PART 2 — Heuristic classifier + ACCURACY GATE (classifier core BULK test-first; wiring WORK; then FULL STOP)

### Classifier core (EngineMath.h — pure, BULK test-first)

```cpp
enum SliceClass { KICK=0, SNARE=1, HAT=2, PERC=3, TONAL=4, OTHER=5 };
struct SliceFeatures {            // = SliceAggregates + context
    float bandRatio[4]; float centroidHz; float zcr;
    float decaySec; float durationSec; float chromaFlatness;
    float onsetStrength;          // 0..1 from transientOnsets
    bool  hasStems; float stemShare[6];  // energy share per stem (sums to 1); zeros if !hasStems
};
struct ClassifyResult { SliceClass cls; float confidence; };
ClassifyResult classifySlice (const SliceFeatures& f, const ClassifierThresholds& t);
```

**Threshold table — ALL tunables live in ONE struct in ONE place** (EngineMath.h, directly above `classifySlice`), with two `constexpr` preset instances `kThreshStems` (tight) and `kThreshNoStems` (wide). Initial values (the gate exists to tune these cheaply — structure is fixed, numbers are not):

| Constant | kThreshStems | kThreshNoStems | Meaning |
|---|---|---|---|
| `drumsDominant` | 0.55 | — | drums stemShare ≥ → percussion subtree |
| `tonalDominant` | 0.55 | — | bass+vox+gtr+pno share ≥ → TONAL |
| `kickLowRatio` | 0.45 | 0.50 | bandRatio[0] ≥ |
| `kickCentroidMax` | 400 Hz | 350 Hz | centroid ≤ |
| `kickDecayMax` | 0.35 s | 0.30 s | decay ≤ |
| `hatHighRatio` | 0.40 | 0.45 | bandRatio[3] ≥ |
| `hatZcrMin` | 0.12 | 0.15 | zcr ≥ |
| `hatDurMax` | 0.35 s | 0.30 s | duration ≤ |
| `snareMidRatio` | 0.50 | 0.55 | bandRatio[1]+bandRatio[2] ≥ |
| `snareFlatMin` | 0.55 | 0.60 | chromaFlatness ≥ (noisy) |
| `tonalFlatMax` | 0.60 | 0.50 | chromaFlatness ≤ (peaked) |
| `tonalDurMin` | 0.20 s | 0.30 s | duration ≥ |
| `minConfidence` | 0.50 | 0.60 | below → OTHER |

**Decision tree (normative structure):**
1. **Stems branch** (`hasStems`): if `stemShare[0] ≥ drumsDominant` → percussion subtree; else if `stemShare[1]+[2]+[3]+[4] ≥ tonalDominant` → TONAL (dominance test + `tonalFlatMax`); else fall through to the spectral tree **with kThreshNoStems** (ambiguous stem mix ⇒ wide thresholds).
2. **Percussion subtree**, evaluated in order KICK → HAT → SNARE, first full-criteria match wins; no match → PERC (confidence = the drums-dominance margin alone).
3. **No-stems spectral tree:** TONAL test first (`tonalFlatMax` + `tonalDurMin`), then KICK → HAT → SNARE, else PERC with confidence from whichever near-miss margin is highest — if that is < `minConfidence`, OTHER.
4. **Confidence (exact, test-pinned):** per-criterion normalized margin `m = clamp((x−t)/max(t,1e-6), 0, 1)` for ≥-criteria and mirrored for ≤-criteria; `confidence = clamp(0.5f + 0.5f * mean(m), 0, 1)` (barely-passing ⇒ 0.5).
5. **Ambiguity rule (binding):** any winning class with `confidence < minConfidence` is demoted to OTHER (keeping the computed confidence). **OTHER is never wrong; a confident wrong guess is the failure mode this rule exists to prevent.**

**AC-P2-core (tests in `tests/ClassifierTests.cpp`, written BEFORE the BULK implementation; reference the named constants, never repeat literals):**
1. Canonical synthetic vectors classify correctly with confidence ≥ minConfidence: kick-like, hat-like, snare-like, tonal-like (via stem dominance AND via no-stems flatness), perc-fallback.
2. Drums-dominance routes to the percussion subtree even when chroma looks tonal (stems-assist beats spectral).
3. Invariant (1000 fixed-seed random vectors, both presets): result ≠ OTHER ⇒ confidence ≥ minConfidence; determinism; all confidences ∈ [0,1].
4. Barely-passing vector yields confidence ≈ 0.5 (±0.01); demotion path produces OTHER.
5. Evaluation-order pin: a vector satisfying both KICK and HAT criteria returns KICK.

### Wiring + gate deliverable (WORK)

- `requestClassifyReport()` sets `wantClassify`, notifies the worker; `doClassifyJob()` (worker): copy `featureFrames` + slice cues under locks → per-slice frame ranges from the sorted assigned cues → `aggregateSliceFeatures` → per-slice `stemShare` by summing squared samples per `stemSet` buffer over the slice range (source-domain, `stemLock`-guarded pointer grab then lock-free read of the refcounted buffers) → `classifySlice` with the preset matching `hasStems()`.
- **Report (a text file — the cheapest honest deliverable):** `Documents\GentSampler\ClassifyReport_<sourceName>_<yyyymmdd-hhmmss>.txt`, then `revealToUser()` on the message thread. Fixed-width table, one row per assigned slice: `slice | pad | time | class | conf | low/mid1/mid2/high | cent | zcr | decay | dur | flat | drums%/tonal% (or "no stems")`, header: file, BPM, stems yes/no + quality, preset used, threshold table values (a tuning round is self-documenting).
- **Trigger:** new item at the bottom of the existing `sliceMenu` popup under a separator — `"Classify slices → report (dev)"`. Classifies **current** slices — does NOT reslice, does NOT touch cues, does NOT pushUndo. Stays in the menu after the gate.
- Empty-source / empty-features guard: report with a single explanatory line, no crash (silent-source landmine).

**AC-P2-wiring:** build/ctest/pluginval green; report generated for a real sample in standalone; report row count == assigned-pad count; classification never runs on the audio or message thread (reviewer-verified); menu item present and dev-marked.

### THE HARD GATE (full STOP — like C5/D6)

**What Joe provides (2–3 real samples, WAV/MP3, roughly 10 s–2 min each):** (1) a drum break, (2) a full mix he'd actually flip, (3) something melodic/tonal with minimal drums. Joe runs these himself (FL or standalone).

**Gate deliverable checklist (all boxes before any Part 3 work):**
- [ ] Build delivered with the dev classify item; one-paragraph run instructions for Joe.
- [ ] Reports generated for all provided samples; the full mix classified **twice** — with stems and without — so the stems edge is visible.
- [ ] Joe checks each row against his ears; disagreements listed as `slice# — heard X, got Y`.
- [ ] Tuning loop: edits touch **only** the `ClassifierThresholds` values. Structure changes (new features, new branches) require a spec amendment.
- [ ] Joe's written approval (sample names + verdict) recorded in the **Gate record** section at the end of this file, and CLAUDE.md updated.

**STOP semantics:** no Part 3 or Part 4 code before that record. Two failed tuning rounds on the same disagreement class → escalate to GATE tier with both reports attached, don't grind.

---

## PART 3 — KIT mapping (after gate; algorithm BULK test-first, wiring WORK)

### Pure mapping (EngineMath.h — BULK test-first)

```cpp
struct KitSlice { int pos; float strength; int cls; };   // cls: SliceClass
std::array<int,16> mapKitPads (std::vector<KitSlice> slices);  // pad → slice index, -1 = empty
```

Normative deterministic rules:
1. **Row convention (by pad index):** pads 0–3 KICK · 4–7 SNARE · 8–11 HAT+PERC · 12–15 TONAL+OTHER.
2. Defensive pre-trim: if input > 16, keep the 16 strongest by `strength` (ties → earlier `pos`, then lower original index).
3. Internal sort by `pos` (ties by original index) — output is **input-order invariant**.
4. Per row: gather its class bucket in time order. Overflow (>4): keep the 4 strongest (ties → earlier pos), place them left-to-right in **time order**; the rest join the spill pool. Underflow: unfilled pads join the empty-pad pool.
5. Spill: pool sorted by time (ties by original index); empty pads ascending by index; assign pairwise until either is exhausted.
6. Invariants: every input slice (post-trim) lands on exactly one pad; nothing silently dropped; same input ⇒ same output.

**AC-P3-core (tests in `tests/KitMapTests.cpp`, written first):**
1. Coverage property: 1000 fixed-seed random inputs — every slice appears exactly once; no duplicates; empties are -1.
2. Determinism + shuffle-invariance: shuffled copies of the same input map identically.
3. Overflow keeps the 4 strongest; the 5th-strongest kick spills; kept kicks appear in time order on pads 0–3.
4. Row purity: a bucket with ≤4 members lands entirely in its own row.
5. Underflow/spill order: constructed case verifying spill fills ascending empty pads in time order.
6. >16 defensive trim keeps overall strongest; empty input → all -1.

### KIT run wiring (WORK)

`sliceKit()` flow, matching the established deferred pattern:
1. Editor (P4 chip): `p.pushUndo()` **once** at click — the single CueSnap for the whole remap (post-0.3: cues, ends, masks, grain = "prior full pad layout"). Then `p.sliceKit()`.
2. Processor: if onsets/features missing → set `kitPending` + `wantAnalysis`, notify, return (mirror of `analysisThenSlice`; `doAnalysisJob` end checks `kitPending` and continues the chain). Else notify the kit chain directly.
3. Worker: boundaries = `computeBlendedSlices()` (SMART reuse; fallback `transientSlices`) → aggregate + classify (stems preset iff `hasStems()`) → `mapKitPads` → apply: assigned pad ⇒ `cues[pad]=pos, cueEnds[pad]=-1`; unassigned ⇒ `cues[pad]=-1, cueEnds[pad]=-1`; `padStemMask` untouched (KIT remaps windows; source selection is orthogonal); `++uiDirty`. Auto ends resolve correctly despite non-monotonic pad→time order because `effectiveCueEnd`'s slice-mode scan is order-independent.
4. No pushUndo anywhere in the worker chain. Ctrl+Z after KIT restores the entire prior layout in one step; redo re-applies.

### Pad class glyph (OPTIONAL)

Tiny K/S/H/T corner glyph on mapped pads, session-only state (`std::array<std::atomic<int>,16> padClass`, -1 default, cleared on load/clear; NOT persisted, NOT snapshotted). **Skip criteria:** needs any new Theme token, competes with existing pad chrome, or the reviewer judges it loud. If skipped, delete the state too.

**AC-P3-wiring:** build/ctest/pluginval green; FL/standalone: KIT on a real mix yields a playable kit laid out by row convention (where material allows); ONE Ctrl+Z restores the prior full layout including masks/grain; redo re-applies; KIT with no source is a safe no-op; audio thread untouched (reviewer-verified).

---

## PART 4 — SLICE split-chip dropdown (WORK)

- **Component:** new `SliceSplitChip` (PluginEditor.h, beside `HeroViewSeg`): one chip, two hit zones — main zone (label) and caret zone (rightmost ~16px, caret per `GentLNF::drawComboBox` idiom). `preferredWidth()` from the shared label font (HeroViewSeg precedent) — the fixed 52px is replaced; label is `"SLICE \xc2\xb7 <MODE>"` (UTF-8 middot literal precedent). Replaces the `sliceMenu` TextButton; must sit clean beside PREVIEW/SNAP/FOLLOW (reviewer inspection).
- **Mode state (processor):** `std::atomic<int> sliceRunMode {0}` — 0 SMART, 1 TRANSIENT, 2 GRID, 3 KIT — and `std::atomic<int> sliceGridBeatsSel {0}` — 0 = ¼ bar (`sliceBeats(1.0)`), 1 = ⅛ bar (0.5), 2 = 1/16 bar (0.25). **Persistence: three-spot IS trivial — do it.** Keys `"slMode"`/`"slBeats"`, clamped on read.
- **Main-zone click = run the current default mode (muscle-memory rule).** Dispatch: SMART → `pushUndo` + `autoSliceMusical()`; TRANSIENT → `pushUndo` + `sliceTransients()` (call the processor directly — exactly one push per run); GRID → `pushUndo` + `sliceBeats(beats)`; KIT → `pushUndo` + `sliceKit()`. Deliberate behavior change from today (plain click currently opens the menu) — Joe's binding design.
- **Caret zone = the styled PopupMenu:** modes section with radio ticks (SMART / TRANSIENT / GRID ▸ ¼·⅛·1/16 / KIT) — **selecting a mode sets it as default AND runs it immediately (one undoable action)**; separator; existing SMART-config submenus unchanged; separator; legacy even-grid submenu + utility items unchanged — zero regression; separator; the dev classify item.
- **Label sync:** on every mode change AND in the 15 Hz `timerCallback` (qualityBox precedent) so a state restore updates the chip; width re-laid-out on label change.

**AC-P4:**
1. Build/ctest/pluginval green.
2. FL validation (Joe): run each of the four modes on a real sample; each produces its layout; Ctrl+Z after each restores the previous layout in one step.
3. Plain click always runs the current default without opening a menu; caret always opens the menu; fresh state defaults to SMART.
4. Mode + grid division survive DAW-session save/reload and kit save/load; chip label shows the restored mode, middot clean.
5. Exactly one history entry per run (no double-push on TRANSIENT — explicit check).
6. All pre-existing menu functionality reachable.

## UI acceptance criteria

1. From a loaded sample, a playable KIT layout is reachable in ≤ 2 interactions (caret → KIT), and in 1 (plain click) once KIT is the default.
2. `SliceSplitChip` visible and enabled whenever the hero chip row is (both hero views), at 1040×700 and the 880×592 floor without truncating any mode label (widest: `SLICE · TRANSIENT`).
3. Caret menu opens targeting the chip and lists exactly 4 modes with the active one ticked.
4. The dev classify item is present at the menu bottom, marked "(dev)".
5. No editor repaint artifacts or hangs while a KIT/classify worker job runs.
6. Optional glyph, if built: legible at floor size, no overlap with pad flag/grip.

## Design source

No Claude Design handoff bundle exists for Phase 3. Visual ground truth = the shipped Phase A–D chip language: Theme.h tokens, `HeroViewSeg`, `GentLNF::drawComboBox` caret, and the `.chip.caret` idiom already ported for `qualityBox`. Any deviation is a spec change, not a judgment call.

---

## Reviewer map (GATE tier)

| Review | Scope | Why GATE / why not lower |
|---|---|---|
| **R0** after 0.3 | snapshot/applySnap extension, write-if-changed, per-gesture push sites, APVTS write path, message-thread-only claim, landmine rewrite | Undo touches every edit action; "undo lies" is invisible to every machine gate (snapshot internals aren't ctest-able). |
| **R1** before the Part-2 Joe gate | P1+P2: `infoLock` discipline, worker purity, no processBlock/message-thread classification, report honesty, Analyzer additive-only diff | ctest covers the pure core but not threading or a lying report; Joe's ear-gate must judge the classifier, not debug plumbing. |
| **R2** after P3 wiring | single-CueSnap semantics, worker chain, atomic cue writes, kitPending, no audio-thread contact, mask-untouched rule | Mapping core machine-checked; wiring failure classes (double-push, worker/undo race) are not. |
| **R3** after P4 | design-language conformance, three-spot completeness, single-push dispatch, timer sync, menu regression | UI + persistence silent-failure classes; pluginval passes a chip that looks wrong or a key in 2 of 3 spots. |

BULK items: reviewer spot-checks that tests reference named constants, not copied literals.

## Landmine flags

1. **Undo-scope landmine ↔ 0.3:** when 0.3 ships, the CLAUDE.md "Undo scope is PARTIAL" landmine is **rewritten, not deleted**: undo covers cue/end + stem-source + all 7 grain params; still NOT covered: per-pad pitch/gain/pan/choke/play-mode/speed and globals; host automation overwrites undone grain values on its next pass (accepted, D-0.3c). BACKLOG entry deleted same commit.
2. **Timer sweep:** the SLICE chip label/mode MUST join the 15 Hz sweep or a restored session shows a stale mode.
3. **UTF-8 middot:** `SLICE · KIT` uses the `"\xc2\xb7"` literal precedent; never through `String::formatted`.
4. **Audio-thread purity:** feature cache and classification on the worker; `processBlock` gains zero reads/allocations.
5. **Silent-source restore × feature cache:** empty restored source → classify/KIT no-op or report-with-explanation, never crash.
6. **pluginval transient flake (0.2):** retry-once, pluginval-only, loudly logged — retry-twice would mask real failures.
7. **`clearCue` resets `padStemMask`** — post-0.3 that's inside the snapshot; KIT deliberately does NOT call `clearCue` for unassigned pads — do not "simplify" to clearCue.
8. **Aborted-dispatch debris:** after any killed agent run, diff against last green commit.
9. **build/ is a junction to D:** — don't "fix" it.

## Tier table (ROUTING.md Rule 1)

| Item | Tier | Justification |
|---|---|---|
| 0.1 CMake Standalone ORT copy | BULK | Machine-caught: build + ls of both DLLs; mirror of a verified block. |
| 0.2 gate.sh retry | WORK | A wrong gate silently passes; fault-injection AC mitigates. |
| 0.3 undo extension | WORK + R0 | "Undo lies" invisible to machine gates. |
| 0.4 P3 breathing (optional) | WORK | Rig bar exists; porting is judgment; skip-friendly. |
| P1 Analyzer frame features | WORK | Perf/memory/lock discipline in the production analysis path. |
| P1 `aggregateSliceFeatures` + tests | BULK (test-first) | Fully specified pure math; tests authored first. |
| P2 `classifySlice` + thresholds + tests | BULK (test-first) | Tree/order/confidence/ambiguity fully pinned; threshold QUALITY is judged at the Joe gate, not by the implementer. |
| P2 wiring (doClassifyJob, report, menu) | WORK | Threading + file IO; a lying report is what no machine catches. |
| P3 `mapKitPads` + property tests | BULK (test-first) | Deterministic combinatorics with exhaustive property tests. |
| P3 wiring (`sliceKit`, single undo, kitPending) | WORK + R2 | Worker/undo interplay is judgment-shaped. |
| P4 split chip | WORK + R3 | UI + persistence silent-failure classes. |
| R0–R3, gate arbitration, escalations | GATE (Fable) | ROUTING Rule 2. |

## Regression fence — do NOT touch

`build/` (junctioned), `processBlock` + the audio path (zero new reads), `StemSeparator.*`, `Transcriber.*`, `ModelDownloader.*`, `Theme.h`, APVTS parameter **layout** (no new params), ORT pin/download blocks (0.1 only appends), `VST3_AUTO_MANIFEST`, `COPY_PLUGIN_AFTER_BUILD`, the deploy merge-copy step, heroView/STEMS view system, `smute*/ssolo*` semantics, `kEnableCuda`, anything pinned. Behavior itches → flag, don't fix.

## Verification commands

```bash
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
bash .claude/hooks/gate.sh                             # build → tests → pluginval(retry-once)
ls "build/GentSampler_artefacts/Release/Standalone/"   # 0.1: both ORT DLLs present
bash -n .claude/hooks/gate.sh                          # 0.2: syntax
```

Plus per-part manual verifies in each AC block, and the Part-2 gate checklist (Joe).

## Risks

1. **Classifier disappoints at the gate** → that's what the gate is for; tuning is constants-only; two failed rounds on one disagreement class → GATE escalation, possibly spec amendment. No UI exists yet — blast radius is a text file.
2. **1024-pt chroma too coarse for low tonal material** → known limit; TONAL leans on stem dominance when stems exist; first lever `tonalFlatMax`, second a spec amendment for a 2048-pt chroma pass.
3. **Undo/APVTS interplay surprises a host** → D-0.3c accepts and documents; write-if-changed minimizes notifications; R0 verifies; fallback (spec amendment) = exclude grain from applySnap, Joe decides.
4. **Worker-chain races** → single worker thread serializes all jobs by construction; R2 verifies; no chain calls pushUndo on the worker.
5. **Split-chip behavior change** (click no longer opens menu) → binding per Joe's doc; FL validation is his own hands.
6. **Feature-cache memory on very long files** → 8 MB cap at 10 min; ~11 MB at 30 min acceptable; downsampling is the pre-approved fallback.
7. **pluginval retry masks a real regression** → pluginval-only, once-only, loudly logged.

## Definition of Done (phase)

- [ ] All Part 0 items committed individually (0.4 done or explicitly skipped with BACKLOG annotation)
- [ ] Build + ctest (54 + new cases) + pluginval strictness 5 green; gate.sh green end-to-end
- [ ] Part-2 Gate record appended to this file with Joe's written approval BEFORE any Part 3/4 commit exists
- [ ] All AC blocks checked; R0–R3 verdicts PASS
- [ ] KIT: one click on a real mix → playable, sensibly-laid kit; one Ctrl+Z restores the prior full layout
- [ ] SLICE split chip matches the design language and survives save/reload with its mode
- [ ] No files outside the file plan; fence intact
- [ ] CLAUDE.md updated; undo landmine rewritten; BACKLOG pruned

---

## Gate record (append at the Part-2 STOP)

| Sample | Stems? | Joe verdict | Notes |
|---|---|---|---|
| | | | |
