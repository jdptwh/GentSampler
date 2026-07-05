# SPEC: Data integrity — restore vs. worker races + async restore decode

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `7f66889`.
**Source:** BACKLOG.md's two HIGH items (sync loadFile-on-construct /
Pad12-ghost family; async-clobber on reopen), taken together per Joe
2026-07-05 ("move to the data integrity items"). Precedes the full pre-package
audit.
**Authored:** 2026-07-05 (Fable planner). Ground truth verified at HEAD.

## Ground rules (inherited)
- No regressions; pluginval-5 green per commit. `processBlock` untouched.
- **NEVER terminate a user application** (CLAUDE.md landmine). Deploy lock
  while FL holds the plugin = report and stop.
- This is the subtlest change of the phase (threading + restore ordering).
  Implementer follows the ordering contracts below EXACTLY; deviations are
  stop-and-report, not judgment calls.

## Code ground truth (verified 2026-07-05)
- `applyStateTree` (cpp ~3400s): `apvts.replaceState` → `loadFile(path,false)`
  SYNCHRONOUS on the calling (message) thread if path exists → cue/end/mask
  restore loop → scalars → stemKey read (+ `wantStemCacheLoad` if no stems) →
  `wantRender=true; notify()`.
- `adoptSourceBuffer` (loadFile's post-decode body) **unconditionally writes
  16-equal default cues + cueEnds=-1** (the `len*i/16` loop) for EVERY caller.
  Restore correctness today depends ONLY on ordering: sync load first,
  restored cues overwrite the defaults after. `runAnalysis==false` (the sole
  restore caller) skips: stemSet clear, stemStatus clear, bpm reset,
  `analysisKeepCues=false; wantAnalysis=true`.
- `doAnalysisJob` completion (cpp ~1140-1160): applies `computeBlendedSlices`
  if `analysisThenSlice`, else `applySlices(res.slices)` if
  `!analysisKeepCues`, then `kitPending` hook. The keepCues check happens AT
  APPLY TIME — an analysis already in flight when a restore lands can pass
  the check after (or race with) the restore's cue writes → **silent clobber
  of restored/hand-edited cues** (BACKLOG HIGH #2).
- The Pad12-ghost family (BACKLOG HIGH #1): stored path may be a stale/empty
  temp export (`GentSampler_Pad12.wav` in tempDir()); restore silently adopts
  a blank source. `gent::pathIsWithin` (EngineMath.h, 9 doctest cases) already
  guards drag-drop against tempDir — restore has no such guard.
- `loadFile` PK-sniffs .gentkit ZIPs → `loadKitV2Audio` (audio-only). Both it
  and plain decode funnel through `adoptSourceBuffer`.

## Change 1 — restore authority (kill the clobber race)
- New `std::atomic<int> restoreGen { 0 }`. `applyStateTree` increments it
  FIRST (before any other work).
- `applyStateTree` also CANCELS all pending derive-then-apply intents:
  `analysisThenSlice=false; kitPending=false; analysisKeepCues=true;`.
  (Restore is authoritative: whatever slicing was queued belongs to the
  pre-restore world.)
- `doAnalysisJob` snapshots `restoreGen` at ENTRY. Every cue-writing apply in
  its completion block (`analysisThenSlice` branch, `!analysisKeepCues`
  branch, `kitPending` hook) is additionally guarded by
  `restoreGen.load() == genAtEntry` — a restore that landed mid-analysis
  makes the job's slice-apply a no-op (analysis DATA — onsets/features/bpm —
  still stores; only the cue apply is suppressed; DBG one line when
  suppressed).
- Same gen-guard on `doSectionApplyJob` and any other worker path that calls
  `applySlices` (audit them all; sliceKit's worker re-entry is covered via
  the kitPending cancel + gen guard).

## Change 2 — async, validated restore decode (kill the ghost + the stall)
- `adoptSourceBuffer` gains `bool keepCues` (default false). When true, the
  16-equal default-cue loop is SKIPPED (everything else identical).
  `loadKitV2Audio` gains and forwards the same param. Existing callers pass
  false — behavior identical.
- `applyStateTree` no longer calls `loadFile` synchronously. Instead:
  1. Validate the stored path FIRST: empty → skip; inside
     `juce::File::getSpecialLocation(tempDirectory)` per `gent::pathIsWithin`
     (canonicalized, the filesDropped guard's exact idiom) → SKIP the load
     entirely + DBG ("restore: refusing our own temp export <path>") — the
     Pad12 ghost dies at the restore door too; not `existsAsFile()` → skip
     (today's behavior).
  2. Stash `restoreLoadPath` (juce::File, under infoLock) + set
     `wantRestoreLoad=true` (notify at the end as today). The message thread
     does ZERO decoding.
- Worker: `if (wantRestoreLoad.exchange(false)) doRestoreLoadJob();` placed
  BEFORE the render/stem-cache dispatches in run()'s loop. `doRestoreLoadJob`:
  read path under infoLock; decode exactly as loadFile does (PK sniff →
  `loadKitV2Audio(f, /*runAnalysis*/false, /*keepCues*/true)`; else
  AudioFormatManager read). **Validate before adopting:** reader null OR
  decoded length <= 0 → DBG + return WITHOUT adopting (never adopt a blank
  source — the ghost's other door). Then
  `adoptSourceBuffer(..., runAnalysis=false, keepCues=true)` and
  `wantRender=true` (render needs the new source; notify()).
- Ordering consequence (the reason keepCues exists): the decode now completes
  AFTER the cue restore — with keepCues=true nothing touches the restored
  cues. `wantStemCacheLoad` is independent (cache decode does not need the
  source) — but run()'s dispatch order must put the restore-load BEFORE the
  stem-cache load in the same wait-cycle so a same-cycle wake handles source
  first (cosmetic, not correctness — stems don't reference source samples).
- `loadKit` (user action, message thread) is UNCHANGED — its synchronous
  loads are user-initiated with a file picker, not the host-restore path.
  Only `applyStateTree`'s path-restore branch changes.
- `getStateInformation` during a pending restore-load: `filePath` is only
  updated when the adopt lands; to keep a save-before-load-completes correct,
  `applyStateTree` ALSO writes the validated stored path into
  `fileName/filePath` (under infoLock) at restore time — display + re-save
  correctness even before (or without) the decode finishing.

## Non-goals
- No change to loadKit/loadFile user paths, saveKit, kit format, stem cache
  semantics, undo, or any editor code. No new tests REQUIRED beyond keeping
  110 green (threading/IO — the logic-only rig can't cover it); if a pure
  decision falls out naturally (it shouldn't), extract+test it.

## Acceptance
- [ ] Reopen a project while an analysis is mid-flight (repro: load a long
  file, immediately reload the project) → restored cues survive; DBG shows
  the suppressed apply.
- [ ] Project reopen does zero message-thread decoding (audible: FL project
  load no longer stalls on big sources; pluginval unchanged-green).
- [ ] Stored path = temp export → restore refuses it; stored path = missing →
  today's behavior; decoded-empty source → not adopted, no blank-wave ghost.
- [ ] Project reopen after a v2 kit load still restores audio (PK sniff on
  the worker path). Save-project-immediately-after-reopen persists the right
  path even if the decode hasn't finished.
- [ ] Normal flows unchanged: fresh load, kit load v1/v2, stem cache restore,
  SECTIONS/KIT slicing, undo. gate.sh green. Joe FL-validates.

## Record
| Change | Commit | Joe verdict |
|---|---|---|
| 1+2 | | |
