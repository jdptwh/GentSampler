# SPEC: AUDIT WAVE 1 — the six HIGH findings (PREPACKAGE_AUDIT.md #1–#6)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `bd55705`.
**Provenance:** drafted by SPEC-DRAFTER from PREPACKAGE_AUDIT.md (audit run
`wf_81cf0f51-134`); reviewed, corrected and OWNED by the PLANNER (Fable,
2026-07-05). All ten drafter open questions are RESOLVED below — the
implementer executes, never reinterprets. **Joe approves this spec before any
implementation** (ROUTING.md loop 1). **APPROVED by Joe 2026-07-05 ("approved.
proceed") — implementation authorized.**
**Numbering:** the audit report's table order is authoritative. Fix N ↔ audit
finding mapping: F1=audit#1(rt-safety pushUndo), F2=audit#2(stale restore
stash), F3=audit#3(exportPad), F4=audit#4(ORT statics), F5=audit#5(download
lock), F6=audit#6(FileChooser liveness). The drafter's mapping table had the
same content under permuted labels; THIS table is final.

## Ground rules
- One fix = one CHECKPOINT COMMIT (Rule 9). Order: F3 → F6 → F2 → F1 → F4 →
  F5. Each checkpoint passes VERIFY_CMD + LINT_CMD (from `.claude/agent.config`)
  before the next fix starts.
- Budgets: MAX_IMPL_ATTEMPTS=2 for F1/F4/F5 (planner-designed mechanics —
  a second failure escalates to PLANNER immediately); default 3 for F2/F3/F6.
  MAX_REVIEW_CYCLES=2 (agent.config).
- Tier: all six IMPLEMENTER (no BULK — drafter's Rule-1 analysis adopted).
- NEVER terminate FL Studio or any user app (CLAUDE.md landmine). pluginval
  "Open plugin (cold)" timeout = real failure, diagnose, never retry-loop.
- Verification-surface note (drafter Q10, planner ruling): F2/F4/F5/F6 fix
  concurrency/lifecycle properties with no automatable regression in the
  ctest harness. ACCEPTED for this wave: reviewer diff-reads the property +
  Joe's manual pass covers the listed repros. Building a multi-instance /
  editor-lifecycle harness is explicitly out of scope (candidate post-wave
  backlog item, filed by the lead at wave close).

## F3 — exportPad() propagates the WAV writer result  (audit #3, order 1st)
`Source/PluginProcessor.cpp` `exportPad` (~2890-2911): capture and return
`writer->writeFromAudioSampleBuffer(...)`'s bool (mirror `encodeFlacTo`'s
in-file pattern). Callers already branch correctly — do not touch them.
**AC:** failure propagates (diff-read); no other line changes; gates green.

## F6 — SafePointer liveness guards on ALL async editor lambdas  (audit #6, 2nd)
**Planner ruling on scope (drafter Q9): EXPANDED.** Same root cause, same
one-line pattern — cover every async completion lambda in `PluginEditor.cpp`
that captures raw `this`:
- The five `FileChooser::launchAsync` sites (loadBtn ~334, saveKitBtn ~351,
  loadKitBtn ~366, exportKitBtn ~379, export-pad-in-menu ~723).
- The FOUR `PopupMenu::showMenuAsync` dispatch callbacks (sliceMenu, kitMenu,
  exportMenu, AND clearBtn ~413 — AMENDMENT W1-A 2026-07-05: the enumeration
  originally missed clearBtn; the implementer caught the inconsistency and
  stopped per protocol; the general scope rule is authoritative, 9 sites
  total). JUCE's ModalComponentManager usually cancels these with r=0 on
  target deletion, but "usually" is not a guard.
Pattern: capture `juce::Component::SafePointer<GentSamplerAudioProcessorEditor>
safeThis (this)`; first statement of each body: `if (safeThis == nullptr)
return;`; access members via the existing code unchanged after the guard
(keep `this` capture alongside for zero-churn bodies, guarded by safeThis).
Destructor stays as-is (SafePointer is the primary and sufficient guard —
drafter's belt-and-suspenders chooser reset NOT adopted; smaller diff wins).
**AC (amended W1-A):** all 9 sites guarded (grep `launchAsync|showMenuAsync`
finds no unguarded `this` deref — the grep-based criterion is literal and
authoritative); normal dialog/menu flows unchanged; gates green incl.
pluginval editor lifecycle.

## F2 — restore stash cleared on EVERY restore + gen-guarded decode  (audit #2, 3rd)
`Source/PluginProcessor.cpp` `applyStateTree` + `doRestoreLoadJob`:
1. At the TOP of applyStateTree's path handling (all branches, including
   empty/refused/missing path): under `infoLock`, `restoreLoadPath =
   juce::File(); wantRestoreLoad = false;` — then the valid-path branch
   re-stashes as today. No restore may inherit a predecessor's pending load.
2. Stash `restoreLoadGenAtStash = restoreGen.load()` (plain int under
   infoLock, written alongside restoreLoadPath). `doRestoreLoadJob` reads it
   with the path and, immediately before `adoptSourceBuffer`, bails (DBG) if
   `restoreGen.load() != restoreLoadGenAtStash` — the doAnalysisJob pattern.
   (Note the decode itself is long; the gen re-check happens AFTER decode,
   right before adopt — a stale decode is discarded, matching the spec'd
   behavior of every other gen-guarded job.)
**Testability (drafter Q4, planner ruling):** no automated case — the harness
cannot drive setStateInformation twice against a live worker. Reviewer
diff-read + Joe manual repro (load project A with source, immediately load
project B with no source → B must not adopt A's audio). Gates green.

## F1 — MIDI tap-to-cue assign deferred off the audio thread  (audit #1, 4th)
**Planner design (drafter Q2/Q3 resolved):**
- Mechanism: `juce::AsyncUpdater` (the processor inherits it privately).
  Chosen over the alternatives for cause: `MessageManager::callAsync`
  ALLOCATES a lambda on the audio thread (rejected — that's the bug class
  being fixed); an editor-timer poll dies when the editor is closed
  (rejected — MIDI assign must work headless). `triggerAsyncUpdate()` is
  JUCE's canonical audio→message signal (pre-allocated internal message;
  no audio-thread heap use).
- Audio-thread side of `handleNoteOn`'s unassigned branch becomes exactly:
  write the assign position into `std::atomic<int> pendingAssignPos[16]`
  (init -1; value = the same previewPlayPos/assignCursor source the current
  code reads — both already atomics) for that pad, then
  `triggerAsyncUpdate()`. NOTHING ELSE — no pushUndo, no setCue, no
  allocation. The per-pad array (not one packed slot) makes near-simultaneous
  assigns on different pads lossless.
- `handleAsyncUpdate()` (message thread): sweep all 16 slots; for each slot
  with pos >= 0, `exchange(-1)`, RE-CHECK `cues[pad].load() < 0` still true
  (drafter Q2.2 — a drag-drop assign may have raced the tick; skip if
  assigned), then run the existing message-thread sequence verbatim:
  `pushUndo(); setCue(pad, pos, /*snap*/false); cueEnds[pad] = kOpenSlice;`
  (the 9f2ab28 point-cue semantics, one undo step per pad assigned).
- `assignPadCue` itself remains a message-thread function for the editor
  drag-drop path — unchanged. `handleNoteOn` simply no longer calls it.
- Latency (drafter Q3, planner ruling): one message-loop tick, VISUAL-only —
  the triggering note never played audio under the current behavior either
  (point-cue assign, no voice start). Accepted; Joe's wave pass includes a
  tap-to-cue feel check as final arbiter.
- Voice-start path for ASSIGNED pads: untouched (drafter Q2.4: no scope creep).
- Correct the false "message thread only" contract comment
  (`PluginProcessor.h` ~355-366) to name the deferred path.
**AC:** no pushUndo/setCue/history access reachable from processBlock's call
graph (grep-verified); audio-thread side = atomic writes + triggerAsyncUpdate
only (diff-read); MIDI tap on unassigned pad still lands a point cue at the
playhead (Joe FL check); simultaneous multi-pad taps all land; race with a
same-tick manual assign resolves to skip; 110+ ctest green; pluginval green.

## F4 — ensureOrtLoaded() once-synchronization  (audit #4, 5th)
**Planner design (drafter Q5/Q6 resolved):**
- Primitive: function-local `static juce::CriticalSection` + `ScopedLock`
  over the ENTIRE body (C++11 magic-static init is itself thread-safe).
  `std::call_once` REJECTED for cause: the body can fail (missing DLL) and
  the current tried/no-retry semantics must be preserved exactly —
  call_once's retry-on-exception semantics don't map to bool-failure.
  SpinLock REJECTED: the body holds LoadLibraryExW for 100ms+ (never spin).
  CriticalSection matches codebase convention.
- Body logic byte-identical inside the lock: `tried`/`g_ortReady` early-out
  preserved (second thread blocks during first's init, then takes the
  early-out with fully-written statics).
- Thread affinity (drafter Q5): all three callers verified worker-thread-only
  (gentCheckStemEngine in run(); initialise in doStemJob; Transcriber's
  gentEnsureOrtLoaded in doTranscriptionJob). The implementer re-verifies
  Transcriber.cpp's call site and STOPS if any non-worker caller exists.
- Cold-open landmine (drafter Q6): the lock lives on worker threads only;
  plugin construction (startThread) never blocks on it. Two instances
  constructing = worker B waits ≤ the CPU-DLL load duration. kEnableCuda
  block untouched.
**AC:** whole-body lock (diff-read); no partial `Ort::Global::api_` ever
observable; kEnableCuda block byte-identical; ctest green; pluginval-5
"Open plugin (cold)" green (landmine check); two-instance FL repro listed in
Joe's manual pass.

## F5 — cross-instance model-download lock  (audit #5, 6th)
**Planner design (drafter Q7/Q8 resolved):**
- Granularity: ONE `juce::InterProcessLock` (Windows named mutex — released
  by the OS on process death, abandoned-mutex safe) named
  `"GentSampler_ModelDownload"` (hardcoded; models dir is not user-
  configurable — path-scoping rejected as speculative), held around the
  whole `ensureModelsPresent` download section (and its Basic Pitch sibling
  if one exists — implementer verifies ModelDownloader.h and covers it with
  the SAME lock). Per-file locking rejected: first-run-only path, simplicity
  wins.
- Acquire pattern: loop `enter(500)`; on each timeout iteration re-check
  `modelsPresent()` (another instance may have finished — return success
  without downloading) and push the status callback string
  `"waiting for another GentSampler to finish downloading models..."`
  (drafter Q8: silent stall rejected). No unbounded single wait; no infinite
  loop either — the wait naturally ends when the holder releases or dies.
- After acquiring: re-check `modelsPresent()` once, then `fileComplete()`
  per file INSIDE the download loop before each `downloadOne` (drafter Q7.5:
  yes, every file).
- No change to downloadOne's resume/SHA-256/atomic-rename internals.
**AC:** no concurrent `.part` writers possible (diff-read of lock scope);
waiting instance shows the status string and completes without re-downloading
finished files; existing single-instance download behavior unchanged; gates
green. Two-instance first-run repro = Joe's manual pass (surface-gap accepted
per Ground rules).

## File plan
| File | Fixes |
|---|---|
| Source/PluginProcessor.cpp | F1, F2, F3 |
| Source/PluginProcessor.h | F1 (AsyncUpdater base, pendingAssignPos, comment), F2 (restoreLoadGenAtStash) |
| Source/PluginEditor.cpp | F6 |
| Source/StemSeparator.cpp | F4 |
| Source/ModelDownloader.cpp (+.h if a constant surfaces) | F5 |
| PREPACKAGE_AUDIT.md | status column per landed fix (same commit as the fix) |
| CLAUDE.md | current-state + landmine entries at WAVE CLOSE (lead's commit, not per-fix) |

## Out of scope
MED/LOW findings #7–#18 (Waves 2/3) — including the ones adjacent to these
edits (#7/#10 inside applyStateTree, #11 render-swap frees, #15 SplitChip
right-click): DO NOT fix "while in there". No undo-format/slot-algorithm
changes beyond relocating the call (0.3-A stays intact). No kEnableCuda/GPU
logic changes. No ORT upgrade. No new dependencies (SafePointer,
InterProcessLock, CriticalSection, AsyncUpdater are existing JUCE 8.0.4).
No persisted-schema changes (restoreLoadGenAtStash is in-memory only).
No new test harness (accepted gap, see Ground rules).

## Verification (every checkpoint)
```
bash .claude/hooks/build_test.sh        # VERIFY_CMD
bash .claude/hooks/pluginval_gate.sh    # LINT_CMD (deploy lock while FL open = expected, report+stop)
```

## Joe's manual pass (wave close, final arbiter)
1. Tap-to-cue feel via MIDI on unassigned pads (F1) — incl. two quick taps on
   different pads.
2. Load project A (with source) then immediately project B (no source) — B
   adopts nothing of A's (F2).
3. Two GentSampler instances in one FL project: both separate stems (F4);
   on a models-absent machine/dir both trigger first-run download (F5 —
   optional if impractical, diff-read accepted).
4. Open each file dialog + the three toolbar menus, close the plugin window
   with them open, then complete/dismiss (F6) — no crash.
5. Export a pad to a read-only folder → visible failure, not silent (F3).

## Record
| Fix | Checkpoint commit | Reviewer verdict | Joe |
|---|---|---|---|
| F3 | | | |
| F6 | | | |
| F2 | | | |
| F1 | | | |
| F4 | | | |
| F5 | | | |
