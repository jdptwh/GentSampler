# PREPACK_UX_SPEC — FINAL (planner-owned; pending Joe approval)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `084d78b` (HEAD; WAVE4 closed, Joe manual pass GREEN 2026-07-06).
**Provenance:** drafted by SPEC-DRAFTER from Joe's two verbatim UX complaints (2026-07-06); reviewed, corrected, and OWNED by the PLANNER 2026-07-06. Sequenced BEFORE the packaging pass. The separate polish pass stays GATED on Joe's explicit word — nothing here opens it.

## Planner rulings (the draft's six open questions)

- **OQ-1 (U1 preserve semantics) — RULING: (a), with a real assign signal.** Snap only when the trigger is a FRESH ASSIGNMENT or a DIFFERENT pad than the previous trigger; a re-trigger of the same pad never moves the view. Rationale: (a) is Joe's verbatim ask ("the pad that's playing… if they trigger it") and preserves FOLLOW's pad-to-pad exploration value, which (b) forecloses; (c)'s manual-zoom dirty flag is an ambiguous state machine (when does "dirty" clear?) that is harder to verify and easy to get stuck in. The draft's hope of avoiding a processor change fails a real corner — clear-cue-then-retap-the-same-pad is a fresh assign that MUST snap, and no view-local state can see it — so U1 adds one atomic assign counter (spec below). Joe's repro confirms the different-pad-still-snaps half; if he wants (b) after feeling it, it is a one-line condition change, escalate back to me only if he asks for (c).
- **OQ-2 (FOLLOW chip) — RULING: contract unchanged.** FOLLOW off = never snaps (exactly today); FOLLOW on = snaps on the narrowed event set above. No relabel, no new chip, no APVTS change. Narrowing what counts as a snap-worthy event underneath the existing gate is the minimal fix; Joe did not ask for a toggle-semantics change.
- **OQ-3 (EngineMath extraction) — RULING: NO extraction, no new doctests.** The only extractable function is `cue + offsetInSamples` — a trivial addition with zero decision content. The real work is each surface's screen→sample conversion, which depends on live view state (`viewStart/viewSpan`, `zoomLo/zoomHi/waveL/waveR`) that does not belong in EngineMath. `effectiveCueEnd`/`resolveEndDragTarget` are untouched, so existing T2.x coverage stands unchanged (116 ctest cases before and after). Per Rule 2 this leaves U2's no-jump property reviewer+Joe-gated — same accepted basis as SLICE_FEEL F1-F5, flagged explicitly.
- **OQ-4 (U2 scope) — RULING: confirmed, END-handle-on-OPEN-slice ONLY.** Verified against the code: CUE anchors from `p.getCue(pad)` (:108, matches its drawn position); the grain marker anchors from `cue + frac*(effectiveEnd-cue)` (:111-116) and is DRAWN from the same formula (strip :1740-1743) — consistent, no jump (AC-F5.1 holds). Non-open END anchors from `effectiveCueEnd` = its drawn position (:1070). Only the open-slice END mismatches drawn vs. anchor. The implementer touches no CUE or grain path.
- **OQ-5 (order) — RULING: U1 → U2, confirmed.** Independent fixes, but U1 first makes Joe's U2 repro honest (re-triggering the pad mid-walk no longer fights his zoom), and U1 sits on the trigger surface WAVE4 F3 just validated — land it while that context is fresh.
- **OQ-6 (WAVE4 cross-reference) — RULING: YES, one line.** U1 adds a bump line directly adjacent to F3's restored block in `handleAsyncUpdate()`; a future auditor diffing that region needs the provenance. One-line forward annotation in WAVE4_SPEC.md's F3 section, added in the U1 checkpoint commit (precedent: WAVE4's own annotation of WAVE1_SPEC.md). Do not otherwise edit the closed record.
- **Draft factual correction (U2):** the draft claimed the bug is "hit identically on both surfaces." False in magnitude. On the HERO the grip is drawn at `cue + 14px` (:1069) while the anchor is `len-1` — the end teleports to the end of the file on first movement (Joe's full complaint: "snaps to the end of the window and then you have to drag it into position"). On the STRIP, an open slice's zoom window already runs to `len-1` (:1565-1568 with `end = effectiveCueEnd`), so the grip at `waveR - 10.0f` (:1574) maps to a sample only ~10px short of the `len-1` anchor — a small offset, not a teleport. Same single root cause (`handleDragBegin` :110), one fix covers both; the hero is the user-visible bug, the strip is a correctness bonus.

## Ground rules (Waves 1-4 protocol, unchanged)

- One fix = one CHECKPOINT COMMIT (`PREPACK_UX #N: ...`), this file's Record row updated in the same commit.
- Gates per checkpoint, from `.claude/agent.config`: `bash .claude/hooks/build_test.sh` (VERIFY_CMD) then `bash .claude/hooks/pluginval_gate.sh` (LINT_CMD).
- The deploy step failing because FL Studio holds GentSampler is a DELIBERATE gate: report and stop. **No agent ever terminates FL Studio or any user application, under any circumstances.**
- A pluginval "Open plugin (cold)" timeout is a REAL failure (2026-07-04 landmine): diagnose, never retry-loop, never dismiss as flake.
- After any aborted/killed dispatch: diff against the last checkpoint before continuing (2026-07-02 landmine).

## U1 — hold hero zoom across pad re-trigger (order 1st)

**Joe verbatim:** "a user needs to be able to zoom into the waveform of the pad that's playing if they trigger it to make adjustments; currently if you hit the pad the waveform zooms back out instead of allowing the user to inspect zoomed in to make adjustments."

**Root cause (verified):** `WaveformView::timerCallback()` (`Source/PluginEditor.h:1347-1361`) snaps the view on ANY `lastTriggerCount` change while `follow` is true (default true, :1467; chip wiring `PluginEditor.cpp:505`, declaration `PluginEditor.h:2829`). `lastTriggerCount` is bumped at exactly three sites (grep-verified, no others): `assignPadCue` (`PluginProcessor.cpp:559-560`, fresh assign — should snap), `handleAsyncUpdate` (:2639-2640, WAVE4 F3's restored fresh-assign parity — should snap), and `startVoice` (:2543-2544, EVERY trigger, including MIDI re-triggers via `handleNoteOn` :2595 and pad-grid clicks via `uiTrigger` `PluginEditor.h:2068` → FIFO → :3967/:3973 → `startVoice`) — the bug. The view cannot currently distinguish assign from re-trigger; U1 gives it the signal.

**Fix:**
1. `Source/PluginProcessor.h` — next to `lastTriggerPad`/`lastTriggerCount` (:307-308), add `std::atomic<int> lastAssignCount { 0 };` with a one-line comment (fresh pad assignment — view-follow snaps unconditionally on this, not on mere re-triggers).
2. `Source/PluginProcessor.cpp` — `++lastAssignCount;` immediately after `++lastTriggerCount;` at BOTH assign sites: `assignPadCue` (:560) and `handleAsyncUpdate` (:2640). `startVoice` (:2543-2544) is UNTOUCHED — that is the point.
3. `Source/PluginEditor.h` `WaveformView` — two new members next to `lastTrig` (:1468): `int lastAssign = -1; int lastTrigPadSeen = -1;`. Rework the :1347-1361 block to:
   - read `trig` and `asg = p.lastAssignCount.load()`;
   - inside the existing `if (trig != lastTrig)` branch: `const bool freshAssign = (asg != lastAssign); const bool newPad = (padIdx != lastTrigPadSeen);` then update `lastAssign = asg; lastTrigPadSeen = padIdx;` UNCONDITIONALLY (even when `follow` is false, so toggling FOLLOW back on doesn't misread the current pad as new);
   - snap (`setView`, existing :1355-1359 body byte-identical) only when `follow && padIdx >= 0 && cachedLen > 0 && (freshAssign || newPad)`.
4. `WAVE4_SPEC.md` — one-line annotation in the F3 section: PREPACK_UX U1 adds `lastAssignCount` beside F3's restored bumps in `handleAsyncUpdate`; F3 behavior (first-tap audition + snap) unchanged.

Accepted, deliberate consequences (do not "fix"): repeated taps of the same pad never re-center the view even after the user scrolls far away or hits FULL VIEW — the view only moves on a new pad, a fresh assign, or a manual gesture. 30 Hz coalescing of multiple triggers into one decision (latest pad wins) is today's behavior, unchanged.

**AC:**
1. Diff shows exactly: one new atomic (declaration :307-308 region), two `++lastAssignCount;` lines (:560, :2640 regions), the reworked timerCallback condition with the two new view members, and the WAVE4_SPEC.md annotation. `startVoice`, `handleNoteOn`, the FOLLOW chip, and the `setView` math body are byte-untouched (reviewer diff-read).
2. Fresh-assign always snaps — including the clear-cue-then-retap-the-same-pad corner (the assign counter, not pad comparison, carries this case; reviewer traces it). WAVE4 F3's first-tap audition parity untouched.
3. `lastAssign`/`lastTrigPadSeen` update even when `follow` is false or `cachedLen == 0` (reviewer checks the update sits outside the snap condition).
4. RT safety: the new counter is an atomic increment only, no allocation, no lock; both bump sites are the existing message-thread assign paths; `processBlock` untouched.
5. Gates green (116 ctest cases, unchanged count).
6. **Joe repro (MANDATORY, feel fix):** (i) zoom into an assigned pad's region in the hero (wheel/scroll, not FULL VIEW), then MIDI-tap and click that same pad repeatedly — the view must not move; drag a cue/end/grain handle mid-taps to confirm adjustments hold. (ii) tap a DIFFERENT assigned pad — view snaps to it (confirm you want this half; if not, say so — it becomes ruling (b), a one-line change). (iii) tap an unassigned pad — assigns, auditions, and snaps exactly as before (WAVE4 F3 feel preserved).

**Not unit-testable** (JUCE timer/view interaction; the atomic is trivial). **Tier: IMPLEMENTER** — the condition placement is judgment; nothing but reviewer+Joe catches a subtly wrong gate. **Budget: MAX_IMPL_ATTEMPTS=2 / MAX_REVIEW_CYCLES=2** — the condition is fully specified above; a second failure means the planner's condition is wrong (spec problem), escalate.

## U2 — open-slice END handle anchors at the drawn grip, not len-1 (order 2nd)

**Joe verbatim:** "when a user drags out the window of a newly auditioned slice it should follow the mouse drag; right now it snaps to the end of the window and then you have to drag it into position."

**Root cause (verified):** `handleDragBegin` (`Source/PluginEditor.h:99-117`) seeds `g.anchorSample = p.getEffectiveCueEnd(pad)` for `Handle::end` (:110). For an OPEN slice, `effectiveCueEnd` returns `len-1` (`EngineMath.h:88-89`), but the grip is DRAWN at `sampleToX(cue) + 14.0f` on the hero (:1069) and at `waveR - 10.0f` on the strip (:1574). First movement in `handleDragMove` computes `proposed = anchor + accum` (:157-159) → `applyEndHandleDrag` (:55-59) → the end lands at ~`len-1`, not at the grip. Hero = full teleport to end-of-file (Joe's complaint); strip = ~10px offset (its open-slice zoom window already ends at len-1). One root cause, three `Handle::end` call sites: hero :856-859 (selected-pad boundary priority), hero :883-887 (`hitEndHandle`), strip :1716-1721. The pure math (`effectiveCueEnd`, `resolveEndDragTarget`, T2.3/T2.4/T2.5) is correct and untouched.

**Fix:**
1. `handleDragBegin` gains one defaulted trailing parameter `int openEndAnchor = -1`. The `Handle::end` seed (:110) becomes: use `openEndAnchor` when `p.isOpenSlice(pad) && openEndAnchor >= 0`, else `p.getEffectiveCueEnd(pad)` as today. CUE and grain branches byte-untouched; all non-end call sites compile unchanged via the default.
2. The three `Handle::end` call sites pass the drawn grip's sample position:
   - hero (:857, :885): `xToSample(juce::roundToInt(endHandleX(pad)))` — reusing the drawing function itself guarantees anchor ≡ drawn grip even if the 14px constant ever changes;
   - strip (:1719): the exact inverse of its `xOf` lambda (:1572) evaluated at the stored `endX` member: `zoomLo + (int) (((double)(endX - waveL) / (double) juce::jmax(1, waveR - waveL)) * zoomSpan)` (guard `waveR > waveL`; `endX` is valid here — the `cueX < 0` early-out at :1711 covers the unpainted/no-region case).
   Callers may pass unconditionally; the `isOpenSlice` gate in step 1 keeps non-open drags byte-identical.
3. NO changes to `handleDragMove`, `applyEndHandleDrag`, `resolveSnap`, `resolveEndDragTarget`, `effectiveCueEnd`, or any EngineMath/test file.
4. Collapse semantics fall out correctly and unchanged: a resolved target within `collapseTolSamples` of cue still collapses back to open (existing `resolveEndDragTarget`/`resolveCueEndEdit` path) — small wiggles on a fresh grip keep the slice open; dragging out past tolerance creates the window under the mouse.

**AC:**
1. Diff = the one-parameter signature change, the gated seed at :110, and the three call sites; a repo grep for `Handle::end` confirms no other `handleDragBegin` end-arm sites exist (census check).
2. Anchor property (reviewer verifies the math per surface): for an open slice, the seeded anchor equals the drawn grip position within ±1px-in-samples at that surface's current zoom (hero: `cue + ~14px`; strip: sample under `waveR-10`).
3. Non-open end drags, cue drags, grain drags: byte-identical behavior (structurally guaranteed by the `isOpenSlice` gate + untouched branches; reviewer confirms).
4. `EngineMath.h` and `tests/` untouched; ctest count stays 116; existing T2.x pass unchanged.
5. Gates green.
6. **Joe repro (MANDATORY, feel fix):** tap an unassigned pad (fresh open slice), then on the HERO grab the small grip just right of the start flag and drag right — the window end must sit under the mouse from the first pixel of movement, no jump to the far end of the file; drag it back near the cue — it collapses back to open within the usual tolerance. Repeat on the SliceDetailStrip's end handle (the grip at the strip's right edge) — end follows the mouse with no initial offset hop. Do this AFTER U1 lands so re-triggering the pad mid-walk doesn't move the hero view.

**Not unit-testable at the live level** (screen→sample conversion needs live view state; per OQ-3 no pure extraction) — reviewer math-check + Joe's hands are the gate, same accepted basis as SLICE_FEEL F1-F5. **Tier: IMPLEMENTER** — a wrong conversion compiles silently and shows only as a residual hop. **Budget: 3/2 (defaults).**

## File plan

| File | Fix |
|---|---|
| Source/PluginEditor.h | U1 (timerCallback block + 2 view members), U2 (handleDragBegin + 3 end call sites) |
| Source/PluginProcessor.h | U1 (one atomic, :307-308 region) |
| Source/PluginProcessor.cpp | U1 (two `++lastAssignCount;` lines) |
| WAVE4_SPEC.md | U1 commit: one-line F3-section forward annotation |
| PREPACK_UX_SPEC.md | Record row per checkpoint (same commit) |
| CLAUDE.md | task close only (lead's commit) |

No other file may be touched. `EngineMath.h` and `tests/SliceWindowTests.cpp` are explicitly NOT in the plan (OQ-3 ruling). No new dependencies.

## Out of scope

- The GATED polish pass — closed until Joe explicitly opens it.
- The packaging pass (CUDA-pack source exclusion, deployed-folder cleanup, installer) — next task, untouched here.
- FOLLOW chip semantics, labels, or persistence; any new UI element, paint change, or layout change.
- Keyboard NUDGE of an open slice's end (`PluginEditor.cpp:1418`, separate path that never calls `handleDragBegin`) — retains today's semantics; if Joe notices it during repro, log it, don't fix it.
- Non-open end drags, start-handle drags, grain-marker drags (verified correct — OQ-4).
- WAVE4 F1-F6 behavior (F3's audition parity in particular), Waves 1-3, `kEnableCuda`/ORT.
- Any `fullView()`/FULL-VIEW reset of the new U1 state — accepted behavior documented under U1.

## Verification commands (every checkpoint)

```
bash .claude/hooks/build_test.sh        # VERIFY_CMD — Release build + ctest (116 cases, count unchanged)
bash .claude/hooks/pluginval_gate.sh    # LINT_CMD — strictness 5; FL-deploy-lock = report and stop
```
Neither fix has a machine-checkable feel property (OQ-3): reviewer diff/math-read + Joe's mandatory FL pass are the real gates, stated per Rule 2 as an accepted reviewer-only surface for the feel half.

## Tier assignment

U1, U2 = IMPLEMENTER (justifications inline above). No BULK — nothing here is machine-verifiable enough to qualify under Rule 1.

## Loop budget

**U1: MAX_IMPL_ATTEMPTS=2** / MAX_REVIEW_CYCLES=2 (condition fully pinned; second failure = spec problem, escalate to planner). **U2: 3/2** (defaults). Budget exhaustion escalates as a spec-sizing problem, never as a request for more attempts.

## Checkpoints (Rule 9 resume points)

1. `PREPACK_UX #1: hold hero zoom across pad re-trigger` — PluginEditor.h + PluginProcessor.{h,cpp} + WAVE4_SPEC.md annotation + Record row.
2. `PREPACK_UX #2: open-slice end handle anchors at the drawn grip, not len-1` — PluginEditor.h + Record row.

Order U1 → U2 (OQ-5). Interrupted work resumes from `git log`/`git status` against this table — never by replaying a prompt; ambiguous partial state rolls back to the last checkpoint.

## Risks

- **U1:** the failure mode that matters is regressing WAVE4 F3's Joe-ratified first-tap snap+audition — the assign counter exists precisely to keep that path unconditional; AC2's clear-then-reassign trace is the corner that catches a lazy implementation. Over-suppression (updating `lastTrigPadSeen` only inside the follow branch) would make FOLLOW-toggle behavior weird — pinned by AC3.
- **U2:** a wrong screen→sample inverse compiles clean and shows only as a residual hop on repro — the reviewer must actually check the strip's inverse against the `xOf` lambda (:1572) and the hero's against `xToSample`/`endHandleX`, not wave it through. Deep-zoom edge: at extreme hero zoom the 14px grip offset is few samples — collapse tolerance then correctly keeps tiny drags open (existing semantics, not a defect).
- **Both:** aborted dispatch → diff against last checkpoint; never terminate FL Studio; pluginval cold-open timeout = real finding. If Joe rejects U1's different-pad-still-snaps half, that is ruling (b) — one-line change, new checkpoint, not a revert of the whole fix.

## Joe's manual pass (task close, final arbiter)

1. U1 (mandatory): the three-part walk in U1 AC6 — same-pad taps hold the view; different-pad tap snaps (confirm wanted); unassigned tap still assigns+auditions+snaps.
2. U2 (mandatory): the hero drag-out walk and the strip walk in U2 AC6.
3. Say explicitly per fix: PASS or what felt wrong.

## Record

| Fix | Commit | Reviewer verdict | Joe |
|---|---|---|---|
| U1 | (this commit) — implementer dispatch completed AC1-4; gate blocked ~40 min by the deliberate FL-lock deploy gate, lead re-ran both gates green after FL released the plugin and committed | — | — |
| U2 | (this commit) | — | — |
