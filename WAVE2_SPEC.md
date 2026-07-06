# SPEC: AUDIT WAVE 2 — the nine MED findings (PREPACKAGE_AUDIT.md #7–#15)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `4941e6a`.
**Provenance:** drafted by SPEC-DRAFTER (all nine findings re-verified by
symbol against current source — premises intact post-Wave-1, current line
numbers throughout); reviewed, corrected and OWNED by the PLANNER (Fable,
2026-07-05). All eight drafter open questions RESOLVED below. **AWAITING
JOE'S APPROVAL — no implementation before sign-off** (ROUTING.md loop 1).
**IDs:** audit finding numbers are permanent IDs. Execution order:
**#7 → #15 → #8 → #13 → #12 → #10 → #9 → #14 → #11** (mechanical first —
adopted from the drafter; all design questions are resolved in this spec, so
nothing blocks the tail).

## Ground rules (Wave 1 protocol carried forward)
- One fix = one CHECKPOINT COMMIT ("WAVE2 #N: ..."), PREPACKAGE_AUDIT.md
  status row in the same commit. Gates per checkpoint: VERIFY_CMD + LINT_CMD
  (`.claude/agent.config`), FL-deploy-lock exception as Wave 1 (compile via
  Tests+Standalone targets + ctest, note it, continue; any other failure is
  real). NEVER terminate FL or any user app.
- Budgets: MAX_IMPL_ATTEMPTS=2 for #9/#14/#11 (planner-designed mechanics);
  3 for the rest. MAX_REVIEW_CYCLES=2.
- Tier: all nine IMPLEMENTER (drafter's Rule-1 table adopted; no BULK).
- Verification surface: #9/#14/#11 concurrency/lifecycle — reviewer diff-read
  + Joe repros (accepted, Wave-1 precedent). #7 ships WITH doctest coverage.

## #7 — padStemMask restore clamp  (1st)
As drafted, drafter recommendation ADOPTED (Q1): new pure
`gent::sanitizeStemMask (int raw) -> std::uint8_t { return (std::uint8_t)(raw & 0x3F); }`
in EngineMath.h (sanitizeHeroView convention), used at the restore site
(PluginProcessor.cpp:3668). Tests in tests/StemMaskTests.cpp: exhaustive
0–255 sweep + named cases (0x40→0x00, 0xFF→0x3F, 0x3F unchanged).
**AC:** helper + call site + tests; nothing else in the loop changes; gates
green.

## #15 — SplitChip left-button gate  (2nd)
Drafter recommendation ADOPTED (Q2): in `SplitChip::mouseUp`
(PluginEditor.h:2715-2729) the gate goes AFTER `down = false; repaint();`
and BEFORE the zone/action branching:
`if (! e.mods.isLeftButtonDown()) return;` — pressed visual always resolves,
only the action is gated. mouseDown untouched.
**AC:** right/middle-click on either zone = no action, no undo push, visual
resolves; left-click identical; Joe repro (right-click chip → layout
unchanged); gates green.

## #8 — length cap on v2 kit stem decode  (3rd)
As drafted: mirror the file's own 10-minute cap pattern
(`maxLen = sr * 600.0; len = jmin(lengthInSamples, maxLen)`) in
`loadKitV2Audio`'s stem loop (PluginProcessor.cpp:3434). The identical LOW
gap in doStemCacheLoadJob (#18) is Wave 3 — DO NOT touch it.
**AC:** stem loop matches the source.flac block above it; #18 site unchanged
(diff-verified); gates green.

## #13 — split report/apply sensitivities  (4th)
As drafted: `sectionSensitivityForReport` + `sectionSensitivityForApply`
atomics; each request writes only its own, each job reads only its own; the
old shared `sectionSensitivity` is REMOVED (grep proves zero refs remain).
**AC:** drafter's four ACs verbatim; Joe repro (report@medium then
apply@few back-to-back → each honors its own); gates green.

## #12 — hybrid manifest indices bounds check  (5th)
As drafted: extend the existing guard (StemSeparator.cpp:888) with
`>= ftOut.numSources` / `>= s6Out.numSources` checks per index, same
"source name mismatch in manifest" error path. FourStem branch: verify
(drafter's read: likely already safe — loop bounded by numSources); extend
only if a parallel gap exists, note which case applied.
**Q on fixtures resolved:** NO synthetic ONNX fixture — diff-read + the
existing error-path precedent is the accepted surface for this fix.
**AC:** drafter's, minus any fixture; gates green.

## #10 — declared defaults on restore fallbacks  (6th)
As drafted: the nine `getProperty(key, <atomic>.load())` fallbacks
(PluginProcessor.cpp:3675-3685) switch to the header's declared defaults via
a small local `constexpr` block placed directly above the nine lines (named,
not magic; reviewer checks each against PluginProcessor.h's initializers).
**Q6 resolved:** NO EngineMath extraction — constant substitution has no
logic to test; the doctest would test the compiler. Reviewer diff-read is
the check.
**AC:** grep shows no `.load())` fallbacks remain in the block; constants
match the header exactly; Joe repro (kit B non-defaults → old v1 kit →
documented defaults land); gates green.

## #9 — stem-cache load cancelled by restore AND direct load  (7th)
**Planner design (Q3 resolved — the drafter's race analysis is correct:
clearing only in applyStateTree does NOT close the window; all three parts
below are required):**
1. applyStateTree's restore-authority reset block also clears, under
   infoLock: `stemCacheKey.clear();` and `wantStemCacheLoad = false;`
   (restore authority — parity with restoreLoadPath).
2. `adoptSourceBuffer`'s `runAnalysis == true` branch (direct user load —
   the branch that already nulls stemSet) also clears both, same locks:
   a genuinely new source invalidates any pending cache load.
3. `doStemCacheLoadJob` gains a KEY RE-CHECK (primary) + gen guard (parity):
   snapshot the key it starts with; immediately before `adoptStemSet`,
   re-read `stemCacheKey` under infoLock — bail (DBG) if it changed or is
   empty. This closes the in-flight window (job already decoding when a
   direct load clears the key → mismatch → discard). Additionally snapshot
   `restoreGen` at job entry and bail on change (doAnalysisJob parity).
   The key re-check is the load-bearing guard; the gen guard is uniformity.
(Q3c confirmed: infoLock covers all stemCacheKey access; no new lock.)
**AC:** all three parts present; the audit's repro (restore-with-key, then
drag a new WAV inside the poll window → new source shows no stale stems)
covered by Joe; DBG on every bail path; gates green.

## #14 — serialize adoptSourceBuffer  (8th)
**Planner design (Q4 resolved): the LOCK, not the worker-queue rerouting,
not gen-based loser detection.** Rationale: every call site decodes BEFORE
calling — the body is assignments/bookkeeping (cheap, bounded hold);
rerouting changes loadFile's synchronous contract and touches editor UX
(disproportionate for a MED); loser-detection still allows interleaved
sub-block writes mid-function, which IS the bug.
- New `juce::CriticalSection adoptLock` (processor member). `ScopedLock` as
  the FIRST statement of `adoptSourceBuffer`, spanning the whole body.
- **Lock-order contract (mandatory, in comments at the member):** adoptLock
  is strictly OUTERMOST and is ONLY ever acquired in adoptSourceBuffer.
  Inside, srcLock/infoLock/stemLock are each taken one-at-a-time (verify:
  never nested with each other in the body — implementer confirms during
  implementation and STOPS if any nesting exists). No code path anywhere
  may take adoptLock while holding any other lock — trivially true since
  adoptSourceBuffer is its only acquirer and no caller holds a lock at the
  call (implementer verifies all three call sites and STOPS if one does).
**AC:** whole-body lock; the lock-order audit note written at the member
declaration listing the verified facts; all three call sites confirmed
lock-free at the call; single-caller behavior unchanged; Joe best-effort
repro (restore + immediate drag-drop); gates green.

## #11 — audio-thread buffer frees deferred (graveyard)  (9th, highest risk)
**Planner design (Q5 resolved; drafter's 18-Ptr worst-case correction
adopted):**
- Pre-sized SPSC ring, processor member:
  `std::array<juce::ReferenceCountedObjectPtr<juce::ReferenceCountedObject>, 64> graveyard;`
  + `std::atomic<int> graveW { 0 }, graveR { 0 };` (single producer = audio
  thread, single consumer = message thread; 64 slots ≫ the 18/block worst
  case, headroom for coalesced ticks). RenderedSample/RenderedStems/
  PadRender all derive from ReferenceCountedObject — base-class Ptr holds
  the reference polymorphically.
- processBlock swap sites (all three, INSIDE their existing try-locked
  scopes — Q5.2: stash and swap are captured together; the contended
  skip-path skips both, unchanged): before each assignment, if the ring has
  a free slot, move the OUTGOING Ptr into `graveyard[w % 64]` then advance
  `graveW`; then assign. **Ring full → SKIP that swap entirely this block**
  (keep playing the old render; the worker's own Ptr still references the
  new one, so the swap simply retries next block — clean backpressure,
  never an inline free, never a block, never a drop).
- Drain: reuse the existing F1 AsyncUpdater — processBlock calls
  `triggerAsyncUpdate()` after stashing (already RT-safe); the existing
  `handleAsyncUpdate()` gains a drain loop: while `graveR != graveW`, null
  the slot (`graveyard[r % 64] = nullptr;` — the free happens HERE, message
  thread) and advance `graveR`. Coalescing is fine: one callback drains
  everything.
- No allocation anywhere on the audio-thread side (array is a fixed member;
  Ptr move into a slot is refcount-neutral). Debug-only thread assertion
  NOT added (drafter's optional idea declined — the diff-read + design make
  it redundant; keep the audio path minimal).
**AC:** no ReferenceCountedObject destructor reachable from processBlock
(diff-read: every outgoing Ptr either enters the ring or the swap is
skipped); stash inside the try-lock scopes; ring full ⇒ swap deferred, not
dropped/freed; drain only in handleAsyncUpdate; no audio-thread
allocation; contended try-lock behavior unchanged; ctest green; pluginval
green (processBlock exercised); Joe repro (rapid tempo/pitch/speed changes,
listening for xruns).

## File plan
| File | Fixes |
|---|---|
| Source/EngineMath.h | #7 |
| tests/StemMaskTests.cpp | #7 |
| Source/PluginEditor.h | #15 |
| Source/PluginProcessor.cpp | #8, #13, #10, #9, #14, #11 (+#7 call site) |
| Source/PluginProcessor.h | #13 (atomics swap), #9 (job guards), #14 (adoptLock + contract comment), #11 (graveyard members) |
| Source/StemSeparator.cpp | #12 |
| PREPACKAGE_AUDIT.md | status rows, same-commit-per-fix |
| CLAUDE.md | wave close only (lead's commit) |

## Out of scope
LOW #16–#18 (Wave 3 — incl. #18's identical cap gap: do not fix while in
there). Wave 1 items (landed). No undo/CueSnap shape changes. No
kEnableCuda/GPU changes, no ORT upgrade, no new deps (all JUCE 8.0.4 /
stdlib). No persisted-schema changes (all new fields in-memory). No
loadFile sync-contract change (#14 ruling). No new test harness beyond #7's
doctest cases.

## Verification (every checkpoint)
```
bash .claude/hooks/build_test.sh
bash .claude/hooks/pluginval_gate.sh
```

## Joe's manual pass (wave close)
The drafter's nine repros adopted verbatim (see draft; notably: right-click
chip no-op; report/apply sensitivity independence; old-kit defaults; stale
stem-cache drag-drop; restore+drag-drop tear check; rapid render-swap xrun
listen). #12 is smoke-only (no corrupt-pack fixture).

## Record
| Fix | Checkpoint commit | Reviewer verdict | Joe |
|---|---|---|---|
| #7 | | | |
| #15 | | | |
| #8 | | | |
| #13 | | | |
| #12 | | | |
| #10 | | | |
| #9 | | | |
| #14 | | | |
| #11 | | | |
