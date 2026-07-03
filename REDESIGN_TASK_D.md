# GentSampler — Redesign Phase D: The COMPOSITE⇄STEMS Stem Map

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ HEAD `56b1583`
**Design source:** `C:\Users\JoeyD\Downloads\gentsampler_redesign_v2.html` — **the mockup is the spec.** The STEMS surface is: the `#viewSeg` segmented control (HTML 248-251, CSS `.seg` 90-102), `drawStems()` (HTML 469-491), `drawFlags()` shared by both views (434-451), and the toggle wiring (494-499). Where prose and mockup disagree, the mockup wins. Where the mockup is **silent**, the answer is a **D1-doc decision for Joe** — never an implementer judgment call.
**Prerequisite:** Phase C complete incl. C6; ctest suite live (TEST_TARGET_TASK.md conventions in force: doctest, `Source/EngineMath.h` `gent::` extractions, gate.sh GATE 2).
**Goal:** Replace today's stacked lanes-band-above-the-wave (PluginEditor.h:301-426 — `bandH` 78, gate `h>60`) with the mockup's design: a COMPOSITE⇄STEMS segmented toggle on the hero that switches between the full-height amber composite and a full-height six-lane Stem Map, flags/cue-region/playhead cutting through both — without losing any shipped capability (stem mute/solo + its persistence, all slice editing, zoom/pan/scrub/preview). After D, the entire face matches the mockup. Full stop.

**Key verified finding driving the phase:** today there is NO COMPOSITE⇄STEMS toggle at all — the build draws a 78px mute/solo lane band stacked above the composite wave whenever stems exist, while the mockup draws two mutually exclusive full-hero views switched by a `.seg` control, and its STEMS view has **no visible mute/solo affordance**. That gap is the spine of D1.

## Ground rules

1. **D1 gates everything.** No D2+ work exists, is started, or is "sketched ahead" until the D1 doc has a reviewer PASS and Joe's written sign-off. The doc's decisions become normative for D2-D6; deviations require re-opening D1, not judgment calls.
2. **View toggle is presentation until D1 says otherwise.** Default model (to be ratified in D1): COMPOSITE⇄STEMS changes what the hero *paints*, and nothing about what any pad *plays*. Playback source selection stays exactly `padStemMask` (PluginProcessor.h:229-237) via `gent::stemGainFor`; global audibility stays `stemMuted/stemSoloed` → `globalAudible[]` (PluginProcessor.cpp:2301-2313). `processBlock` is untouched this phase.
3. **Single-edit-path invariant.** Flags, cue region, handles, scrub, zoom/pan in the STEMS view route through the SAME hit-tests and drag engine as the composite view. No second editing path, no duplicated hit constants.
4. **Suite grows with the phase.** Every task that adds or moves engine-adjacent state ships its EngineMath extraction + doctest coverage in the same diff (per-task lists below). `ctest --test-dir build -C Release --output-on-failure --no-tests=error` must be green at every task boundary.
5. **Side-by-side method (mandatory):** headless-Chrome mockup at 100% vs standalone at 1040 client width, per-region crops, for BOTH views. The sbs capture is the arbiter of all mockup-literal ambiguity (note: the mockup canvas is 2032×320 backing for a 1040×160 element — `drawStems()` literals are ~2× canvas-space; follow the interpretation convention Phase C already established for `drawFlags`/`drawComposite`, then let the sbs settle residuals).
6. Phase A/B chrome conventions bind any new hero chrome: seg = the established `.seg` primitive (TrigPad pattern), Theme fonts at the ×1.12 rule, adaptive `chipGlowMargin`, overlay chip kind 3 for anything drawn over wave content, **never** set `buttonColourId` on a chip (active-tint trap), UTF-8 literals concatenated — never inside `String::formatted`.
7. Build via cmake; commit per task; **stop at the D6 gate**; FL validation is final acceptance.

---

## D1 — STEM VIEW STATE MODEL (design doc, NOT code) — REVIEWER PASS + JOE GATE

**Deliverable:** `docs/STEM_VIEW_MODEL.md` in the repo. Zero code changes. The doc must be executable-spec-grade: an implementer who never saw this conversation could build D2-D5 from it. Every claim about current behavior cites a real symbol and file:line at HEAD; every mockup claim cites the HTML line; **every gap between the two is surfaced as a numbered DECISION with a recommended default, for Joe to accept or override.**

**Required sections (a thin doc FAILS review; all nine are mandatory):**

1. **Current-state inventory (code ground truth).** What exists today, cited: the always-on lanes band and its geometry/gates (`showStems`, `stemsShowing && h > 60`, `bandH = jlimit(78,250,…)` — PluginEditor.h:301-328); lane paint incl. mute pill + hover-solo box (356-426); lane hit-tests and their early-return priority in `mouseDown` (730-740) and `mouseMove` hover (942-944); `stemPeaks` population (1099-1133); processor state: `padStemMask` (h:438), `stemMuted/stemSoloed` (h:436-437), three-spot persistence keys `smute*/ssolo*` (cpp:2129-2130, 2192-2193, 2758-2759); `gent::stemGainFor` + `globalAudible` derivation (cpp:2301-2313, 2382); preview honoring mute/solo (cpp:2644-2686); async separation lifecycle (`stemStatus`, `renderedStems`, per-pad stem renders w/ `stemRenderGen`).
2. **Mockup ground truth.** What `#viewSeg` + `drawStems()` actually specify: mutually exclusive full-hero views; STEMS = 6 lanes at `laneH = H/6`, alternating lane bed `rgba(255,255,255,.015)`, 1px `rgba(0,0,0,.45)` separators, stem-colour columns at alpha .8 starting right of a `rgba(10,13,16,.85)` label plate, stem-colour bold mono lane labels; `drawFlags` (numbered stem-hued flags, amber cue region, glowing playhead) runs full-height in BOTH views; composite view has NO lanes band; STEMS view has NO time ruler; **no mute/solo affordance is drawn anywhere in the mockup**.
3. **Mode semantics.** What COMPOSITE vs STEMS MEANS: for playback (default: nothing — pure view), for per-pad source selection (default: nothing — SOURCE chips remain the only `padStemMask` writer), for preview/audition, and for the pad grid/inspector (unaffected). State explicitly what a lane switch does mid-playback (default: repaint only; zero audio-thread interaction — the toggle is read on the message thread only).
4. **State model.** Where view mode lives (default: `std::atomic<int> heroView` on the processor, editor-close-proof per the `edW` precedent), legal values, who reads/writes, sanitization of out-of-range stored values.
5. **Persistence.** Whether view mode persists and where (DECISION: project state via the three-spot pattern `getStateInformation`/`saveKit`/`applyStateTree`, vs session-only, vs not at all — recommend three-spot, key `heroView`); interaction with kit save/load; **and the missing-source case**: the standalone restores last-loaded file, which can come back silent/empty — define restore behavior when persisted view = STEMS but the restored source has no stems (default: request is sticky, effective view resolves to composite/empty-state until stems exist; seg reflects the effective view — DECISION which of request/effective the seg shows).
6. **Async separation lifecycle × view.** The full matrix: no source loaded / source loaded + never separated / downloading models / separating N% / stems ready / separation failed / stems present from restore. For each: what the STEMS seg does (enabled? disabled? placeholder lanes?) and what the hero paints. Mockup is silent pre-separation — DECISION with recommendation (e.g., seg enabled, STEMS shows the six lane plates + "SEPARATE STEMS to fill the map" placeholder wired to `sepStemsBtn`; auto-nothing on completion — no surprise view flips).
7. **Mute/solo relocation (the big silent gap).** Today mute (label pill) and solo (hover S box) live in the always-on band — visible in what is effectively the composite view. The mockup's composite has no lanes and its STEMS view draws no mute/solo. Shipped, persisted functionality may not be lost. DECISION: where the affordances live post-D (recommend: carry current semantics into the full lanes — label plate click = mute, hover S box at lane right = solo), and whether mute/solo being reachable *only* inside STEMS view is acceptable UX regression for composite-dwellers.
8. **Interaction map for STEMS view.** Which composite interactions remain live in STEMS (recommend ALL: flag click-select, cue/end handle drags, scrub, wheel zoom, shift/middle pan, scrollbar) and exact hit-test priority (lane plate/solo zones claim clicks ONLY within their x-ranges; everything else falls through to the shared paths — today's whole-band early return at PluginEditor.h:730 dies). Undo/slices/grain: state explicitly that slice edits made in STEMS view are the same `pushUndo`/CueSnap edits; grain marker stays a SliceDetailStrip affordance (mockup shows none in lanes).
9. **Undo scope statement.** View toggle and mute/solo are NOT undoable today; `padStemMask` is not either (BACKLOG extend-undo item). Position D explicitly: D adds only view state (not undo-worthy by convention) → **D does not depend on, and does not advance, the BACKLOG undo item**; if any Joe decision in §7/§8 adds a `padStemMask`-writing affordance, that decision must simultaneously resolve its undo story or be rejected. Also: geometry retirement list (the `bandH` 78 floor, `h>60`/content gate, `stemBandTop/H` members) and the answer to the fit question — **the mockup's lane design fits the 160px hero**: lanes REPLACE the wave (6 × ~26.7px at 1040×700; ~22.5px at the 880×592 floor under aspect-lock — both beat today's 13px band lanes). No new geometry needed; the stems view claims the full hero.

**D1 reviewer pass (GATE):** verdict on the DOCUMENT — completeness vs the nine sections, internal consistency, contradictions with cited code, unstated assumptions, every mockup-silence converted into a numbered DECISION. Verdict format per ROUTING.md.
**JOE GATE:** Joe accepts/overrides each DECISION in writing. **D2 does not exist until this is done.**

**D1 acceptance criteria:** doc exists at `docs/STEM_VIEW_MODEL.md`; all nine sections present; ≥1 file:line citation per current-behavior claim; every DECISION numbered with a recommended default; reviewer PASS; Joe sign-off recorded in the doc header.

---

## D2 — View-mode state + COMPOSITE⇄STEMS seg control

**Scope (per ratified D1):** `heroView` state on the processor + sanitization; three-spot persistence (if D1 §5 ratifies it) with keys alongside `smute*/ssolo*` (cpp:2129/2192/2758 neighborhoods); the seg control in the hero's `.ov.tr` — mockup order: seg THEN the HQ caret-chip (HTML 247-252); build tr currently holds only `qualityBox` at `in.getRight()-58` (PluginEditor.cpp:902-903) — seg slots to its left; seg styled via the established segmented primitive; toggle wires to repaint only. No lane painting yet (D3) — in this task STEMS position may paint the D1-ratified placeholder.

**ctest additions (same diff):**
- `gent::resolveHeroView (int requested, bool stemsAvailable)` → effective view; tests: requested STEMS + no stems → composite; + stems → stems; composite always composite.
- `gent::sanitizeHeroView (int stored)` → clamp to legal values, unknown → composite; tests incl. negative/large/garbage ints (the missing-source restore path).
- T4-pattern transition sequence test: scripted `(requested, stemsAvailable)` sequences — stems arriving mid-session flips effective to the sticky request; stems vanishing (new file loaded) falls back — matching D1 §5/§6 exactly.

**Acceptance criteria:** seg renders in `.ov.tr` matching mockup (sbs crop); clicking toggles state and repaints; state survives editor close/reopen; if persisted: save→reload round-trips, old projects without the key default composite; new tests green; no APVTS change; no audio-thread read of the new atomic.

## D3 — STEMS view paint + composite band retirement

**Scope:** Port `drawStems()` (HTML 469-491) into `WaveformView::paint` as the full-hero STEMS branch: `laneH = h_wave/6`, alternating lane beds, separators, per-stem colour columns from the existing `stemPeaks`, label plates + stem-hued mono labels; `drawFlags` equivalents (flags, active cue region, playhead — already built in C2) render full-height across lanes; no top/bottom ruler in STEMS (mockup); mute/solo affordances painted per D1 §7. COMPOSITE branch: delete the lanes band entirely — composite reverts to the existing `bandH==0` layout (full-height wave + its bottom ruler, PluginEditor.h:682). Retire `bandH` jlimit/floor-78, the `h>60` content gate as stems-band logic, and repurpose `stemBandTop/H` members per D1 §9. Scrollbar band + `bottomChromeInset` behavior unchanged in both views. Do NOT touch the benched P3 `waveRamp` territory (BACKLOG) — composite wave paint stays byte-identical.

**ctest additions (same diff):**
- `gent::laneIndexAt (int y, int bandTop, int bandH)` — extract the duplicated inline formula (PluginEditor.h:732, 943); tests: boundary y at every lane edge, out-of-band clamping semantics identical to today's `jlimit`.
- Lane-tiling property test: for band heights {96,135,160,250} all y in-band map to exactly one lane, lanes tile with no gap/overlap, `laneIndexAt` consistent with computed lane tops.

**Acceptance criteria:** sbs STEMS-view crop vs mockup passes (lane beds, separators, plates, colours, flags-through-lanes); sbs composite crop shows NO band and is otherwise unchanged vs the C6-era composite capture; empty-state and placeholder render per D1 §6; both views clean at 1040×700 and the 880×592 floor; tests green.

## D4 — Interaction rewire (the blast-radius task)

**Scope:** Replace the whole-band early-return hit-test (PluginEditor.h:730-740) with D1 §8's map: in STEMS view, lane mute/solo hits claim ONLY their x-zones (plate / S-box) at their lane's y; every other click/drag/wheel falls through to the SAME existing paths — flag select (763-779), handle drags, scrub, zoom, pan, scrollbar (742-750). Hover-solo reveal (`hoverLane`, 942-949) rewired to full-lane geometry. In COMPOSITE view, zero lane hit-tests exist. Timer sweep for the seg + any new hover state (the B/C timer-stomp lesson). Tooltips/cursors per existing conventions.

**ctest additions (same diff):**
- `gent::laneZoneAt (int x, int w, int labW, int soloW)` → {mute, solo, wave} classification extracted from the 733-736 constants; tests: exact boundary pixels both sides of both zones, degenerate small w.
- Composition test: `laneIndexAt` × `laneZoneAt` over a scripted click grid reproduces today's toggle targets for the same relative geometry (behavior-identical claim for the zones that survive).

**Acceptance criteria:** in STEMS view — mute/solo toggle exactly as before (incl. persistence untouched); a flag click selects its pad; cue/end handle drag edits the real window with snap + undo (same `pushUndo` path — verified by undoing a STEMS-view edit); wheel zoom/pan/scrub/scrollbar all live; in COMPOSITE view no dead zones remain where the band was; tests green; pluginval green.

## D5 — Async lifecycle + degraded states

**Scope:** Implement the full D1 §6 matrix: seg/hero behavior across no-source, pre-separation, downloading, separating, ready, failed, and restore-with-missing/empty-source (standalone silent-source landmine). Sticky-request semantics per D1 §5 via `resolveHeroView` — no code duplicates the predicate. `stemStatusLbl` interplay per D1 (it already narrates the lifecycle — PluginEditor.cpp:1560-1570). New-file load resets stems availability without nuking the view request.

**ctest additions (same diff):** extend the D2 transition-sequence test to the FULL D1 §6 matrix as scripted predicate sequences (every row of the matrix is a named TEST_CASE asserting the effective view + seg-enabled flag pair).

**Acceptance criteria:** each matrix row demonstrably correct in the standalone (checklist in completion notes); kill-case: persisted STEMS + missing source on standalone relaunch → no crash, composite/empty renders, STEMS engages by itself once stems return; tests green.

## D6 — GATE: captures, review, FL, commit

Deliverables: sbs pairs `sbsD_stems.png` / `sbsD_composite.png` (1040) + floor shots (880×592) + a mid-separation shot + a pre-separation placeholder shot; final reviewer full-diff pass; Joe validates in FL (stale-binary trap: rebuild/reinstall first); commit; CLAUDE.md "Current state" updated (Phase D complete — full face matches mockup). **Capture rig notes (established lessons):** headless Chrome can't click — capture the mockup's STEMS state from a scratchpad COPY of the HTML with `drawStems()` invoked at load (one-line swap; never edit the Downloads original); build-side, locate the standalone window by the **launched process PID** (not title) and `SetWindowPos` TOPMOST before shooting, or another window's pixels get grabbed; put the build into STEMS via a synthetic click at the seg's client coords derived from `resized()`, and verify a lane-plate pixel before capturing.

---

## Reviewer placement map (with justifications)

| Point | Placed? | Justification |
|---|---|---|
| **R1 — after D1 (doc)** | YES (GATE) | Mandated. A wrong state model is caught by nothing downstream and poisons four tasks — the definitional GATE case. |
| after D2 | YES (GATE, small) | Three-spot persistence has a *silent* failure class — a missed spot drops state from kits with no machine check (gotchas memory: kits "silently drop" single-spot state). ctest can't see ValueTree plumbing. Cheap targeted pass on the persistence + state diff only. |
| after D3 | NO dedicated pass | Paint-only surface whose arbiter is the sbs capture + Joe's eyes at D6; a wrong lane bed fails visibly, not silently. Its engine-adjacent piece (`laneIndexAt` extraction) is machine-checked by the behavior-identical tests. Folding it into R3 avoids a third full-context spin-up. |
| **R3 — after D4+D5, before D6 captures** | YES (GATE, full diff D3-D5) | D4 is the phase's blast radius: hit-test re-priorization can kill slice editing or mute/solo, and *nothing automatic clicks the UI* — pluginval opens the editor but doesn't exercise mouse paths. Reviewer verifies: single-edit-path invariant (no duplicated drag/undo logic), hit-priority matches D1 §8, no audio-thread reads of `heroView`, retirement list complete (no orphaned `bandH` logic), fence compliance. |
| after D5 alone | NO | D5's failure modes are enumerable state-matrix rows — exactly what the D5 ctest cases machine-check; residual UI wiring is inside R3's diff anyway. |
| **D6 — Joe in FL** | YES (human) | Project convention: FL validation is final acceptance; also the only real-host check on editor interaction. |

## Landmine flags (interactions with known mines)

1. **Undo-scope gap (BACKLOG extend-undo):** D adds view state only — not undo-worthy; mute/solo/`padStemMask` remain outside CueSnap exactly as today. **D neither depends on nor advances the BACKLOG item.** Tripwire: any D1 §7/§8 decision that lets lanes WRITE `padStemMask` re-opens this — that decision must carry its own undo resolution or be rejected at the Joe gate.
2. **Standalone silent-source restore:** persisted STEMS + missing/empty restored source → `resolveHeroView` falls back to composite/empty; sticky request; machine-checked in D2/D5 tests; kill-case demo in D5 AC.
3. **Whole-band early-return** (PluginEditor.h:730): if carried into a full-height lanes view it makes ALL slice editing dead in STEMS — the exact class of the B6 "unreachable mute/solo lanes" finding, inverted. D4's core job; R3 verifies.
4. **Mute/solo reachability regression:** post-D they exist only inside STEMS view (mockup's composite has no lanes). Surfaced as D1 DECISION-§7, not slipped in.
5. **Async separation lifecycle:** availability flips on a background thread mid-session; view resolution must be message-thread-only reads of existing atomics/status — no new locking; no surprise auto view-flips unless D1 says so.
6. **Timer-stomp pattern:** the 30Hz `timerCallback` has a history of clobbering styled controls (A4/B gotchas) — D4 sweeps the seg + hover state.
7. **Chrome conventions:** no `buttonColourId` on the seg (active-tint trap); adaptive `chipGlowMargin`; UTF-8 never inside `String::formatted`.
8. **Benched P3 (BACKLOG):** D3 repaints *around* the composite wave loop — the `waveRamp` alpha-loss retry stays benched; composite columns byte-identical.
9. **Mockup canvas 2× backing scale** (2032×320 vs 1040×160): literal-px ambiguity — follow Phase C's established interpretation, sbs arbitrates.
10. **Stale-binary trap** before FL validation; **ORT/CUDA fences** untouched as always.

## Tier table

| Item | Tier | Justification |
|---|---|---|
| D1 doc authorship | WORK (Sonnet) | Judgment-shaped synthesis grounded in code reading; R1 (GATE) + Joe catch failures. |
| R1 / R2 / R3 reviews | GATE (Fable) | ROUTING Rule 2 — gates. |
| D2 state/persistence/seg | WORK | Persistence + UI wiring failures are silent to machines. |
| D2/D3/D4/D5 **pure `gent::` functions, test-first** | **BULK (Haiku) — conditional** | The ONLY BULK-eligible work this phase, and only under this protocol: Sonnet authors the failing doctest cases + exact signatures FIRST and commits them; Haiku implements `resolveHeroView` / `sanitizeHeroView` / `laneIndexAt` / `laneZoneAt` until green. Machine check that catches a wrong implementation: the pre-written failing tests + compiler (`--no-tests=error` prevents fake green). If Haiku touches any file other than `Source/EngineMath.h` + the named test file, auto-FAIL. Two failures → WORK per Rule 4. |
| D3 paint / D4 interaction / D5 lifecycle | WORK | Paint fidelity, hit-test priority, and state-matrix wiring are judgment-shaped; nothing machine-clicks the UI. |
| Capture rig runs | WORK (same task) | Mechanical, but the rig has silent-failure history (wrong-window capture) — needs an operator who checks what it shot. |
| Final acceptance | Joe (FL) | Convention. |

Explicitly NOT BULK: seg styling ("just CSS-like" but active-tint/glow-margin traps are invisible to machines), persistence plumbing, any `PluginEditor.h` edit.

## Regression fence (out of scope — explicit)

`processBlock` and the entire audio path (the `globalAudible`/`stemGainFor`/`prevStemSg` machinery, cpp:2301-2686) — untouched; `padStemMask` semantics and SOURCE chips; `smute*/ssolo*` keys and values; CueSnap/undo internals; `SliceDetailStrip` (all of it); composite wave column loop + bottom ruler; snap engine; header (X-budget stays closed); pad grid; inspector; separation/transcription/ModelDownloader/`StemSeparator.*`; ORT pin, `kEnableCuda`, `VST3_AUTO_MANIFEST`, POST_BUILD steps, `build/`; plugin-target CMake lines (test additions ride the existing guarded block); no new dependencies; any behavior itch outside D1's ratified decisions → REPORT, don't touch.

## Verification

```
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
bash .claude/hooks/gate.sh          # build → tests → pluginval strictness 5
```
Plus per D6: sbs capture set (stems/composite/floor/mid-separation/placeholder) and Joe's FL validation.

**UI acceptance criteria** (JUCE editor — pluginval + reviewer/Joe inspection stand in for Playwright): COMPOSITE⇄STEMS reachable in 1 click from the default view; seg visible/enabled per the D1 §6 matrix at every lifecycle stage; in STEMS view a slice edit (flag select → handle drag → Ctrl+Z) completes with zero dead zones; both views artifact-free at 1040×700 and 880×592; pluginval strictness 5 green throughout.

## Definition of Done

- [ ] D1 doc reviewer PASS + Joe sign-off recorded (before any D2 code existed)
- [ ] Build + ctest + pluginval green at every task boundary; suite grew in D2, D3, D4, D5
- [ ] All task ACs checked; R2 + R3 reviewer PASS
- [ ] sbs pairs accepted (both views, both sizes); no file outside per-task plans
- [ ] Joe validates in FL (fresh install)
- [ ] Commits per task; CLAUDE.md "Current state" updated — Phase D complete, face matches mockup

---
*Spec authored by planner (Fable) 2026-07-03 @ HEAD 56b1583. Key verified facts: no view toggle exists today (band always-on, PluginEditor.h:301-328); mockup toggle is view-only JS (HTML 494-499); mockup draws no mute/solo anywhere; mute/solo + persistence are shipped state (cpp:2129/2192/2758); EngineMath/ctest suite live. All mockup silences are D1 DECISIONs — none delegated to implementers.*
