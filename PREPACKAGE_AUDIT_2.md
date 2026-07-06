# PREPACKAGE AUDIT 2 — 2026-07-06

Second full multi-agent audit pass (Joe-requested), run after the 18/18
burn-down of PREPACKAGE_AUDIT.md and Joe's Wave-3 smoke PASS. Same shape as
the first run: read-only subsystem auditors -> dedup -> one adversarial
verifier per finding (default stance: refute with code evidence; confirm only
with a concrete user-triggerable failure walk). Two NEW lanes this pass:
regression-diff (auditing only `29969f7..HEAD` — the wave 1-3 / DATA
INTEGRITY / ADDENDUM T fixes themselves) and build-packaging (blockers for
the upcoming installer task).

Run wf_90cc8154-5bd: 18 agents (10 auditors + dedup + verifiers, all Opus).
7 raw -> 7 deduped -> **6 CONFIRMED (0 HIGH / 5 MED / 1 LOW), 1 refuted**.
Repo @ `6af11aa`. Baseline build + ctest (116 cases) green before the run.
Lanes rt-safety, file-io, stem-engine, memory, ui-wiring(partial), engine-math
returned zero new findings — the wave 1-3 hardening held up under re-audit.

| # | Sev | Subsystem | Finding | Where | Status |
|---|-----|-----------|---------|-------|--------|
| 1 | MED | threading | doStemJob publishes stemSet/stemCacheKey with no restoreGen guard — a restore landing mid-separation gets the previous song's stems attached to the new source | Source/PluginProcessor.cpp:1445,1456 | FIXED WAVE4 F1 (gen + source-identity guard, both publish sites) |
| 2 | MED | ui-wiring | Incoming pad-range MIDI note re-binds every inspector SliderAttachment mid-drag, hijacking an in-progress knob edit onto a different pad | Source/PluginEditor.cpp:1545 + Source/PluginProcessor.cpp:2533 | FIXED WAVE4 F2 |
| 3 | MED | regression-diff | WAVE1 F1 fix dropped the audition voice-start on MIDI-tapping an unassigned pad — first tap is now silent (spec accepted the deferral on a FALSE premise) | Source/PluginProcessor.cpp:2572 vs baseline :563 | FIXED WAVE4 F3 (pending Joe feel re-check) |
| 4 | MED | build-packaging | CMake POST_BUILD deploy hard-fails the whole build (or silently writes a malformed bundle) on any machine without the pre-existing writable Program Files folder | CMakeLists.txt:141-145 | OPEN |
| 5 | MED | build-packaging | build.bat `xcopy /e` ships the ~2.6 GB dev-box CUDA/cuDNN pack verbatim into the user's VST3 folder (already mirrored on the release-cutting box) | build.bat:47 | OPEN |
| 6 | LOW | persistence | doStemRenderJob resample branch OOB heap read when a kit/cache stem's sample rate differs from the source's | Source/PluginProcessor.cpp:2182 | OPEN |

Refuted (1): engine-math claim that applyCueEdit's end-collapse tolerance
uses raw samplePos vs clamped cue — arithmetic quirk is real but the required
state (cue<0 with small positive end + armed drag) is UNREACHABLE: every drag
arm path (SliceDetailStrip mouseDown PluginEditor.h:1710, hitEndHandle
:1076, hitStartEdge :1060) hard-requires `getCue >= 0`.

---

## 1. [MED] doStemJob publishes stemSet + stemCacheKey with no restoreGen guard
**Where:** `Source/PluginProcessor.cpp:1445` (stemSet publish), `:1456` (stemCacheKey)  **Subsystem:** threading

**Failure walk (verifier-confirmed end to end):**
1. User loads song A, runs stem separation. doStemJob (:1299) captures src=A
   at :1301, sets separating=true (:1311), blocks in stemEngine.separate()
   (:1419) for minutes. doStemJob has NO `genAtEntry = restoreGen.load()`.
2. Host reopens a project referencing song B with no stem-cache key:
   applyStateTree (:3666) bumps restoreGen (:3673), cancels
   analysisThenSlice/kitPending/wantRestoreLoad/wantStemCacheLoad
   (:3674-3694), stashes restoreLoadPath=B. It never touches
   separating/wantStems/stemSet — the in-flight separation is neither
   cancelled nor invalidated. restoredStemKey empty -> wantStemCacheLoad
   stays false.
3. Worker is single-threaded (:947-1005), so wantRestoreLoad waits.
   Separation finishes: stemSet=A published under stemLock (:1445-1448)
   and stemCacheKey=computeStemKey(A) written (:1456/:1496) with no
   restoreGen re-check; wantStemRender=true (:1558).
4. doRestoreLoadJob then adopts source B via adoptSourceBuffer(...,
   runAnalysis=false, keepCues=true) (:3550) — the stemSet clear is gated
   `if (runAnalysis)` (:325-329), so A's stems SURVIVE.
5. doStemRenderJob (:2141) stretches A's 6 stems into B's SR/length domain
   and publishes them as B's rendered stems.

**Result:** B reports hasStems()==true with song A's stems; STEMS view and
padStemMask routing play A's stems under B; stemCacheKey mis-associates A's
cache with B. Same restore-authority class as audit-1 #5/#9/#14, but
doStemJob's own publish was never gen-guarded (doAnalysisJob :1182,
doSectionApplyJob :2067, doRestoreLoadJob :3544, doStemCacheLoadJob
:3594/:3636 all have the guard — verified asymmetry). STEM_VIEW_MODEL.md
DECISION-6 does not cover this case (it assumes stems arrive from a matching
key or a fresh separation of the restored source).

**Fix sketch:** Snapshot `const int genAtEntry = restoreGen.load();` at
doStemJob entry (near :1301); before BOTH publishes (:1445-1448 and
:1456/:1496/:1558) re-check and on mismatch discard the StemSet, skip the
cache write and wantStemRender, clear separating, ++uiDirty, return — the
exact doStemCacheLoadJob (:3636) / doRestoreLoadJob (:3544) bail pattern.

## 2. [MED] Pad-range MIDI note re-binds inspector SliderAttachments mid-drag
**Where:** `Source/PluginEditor.cpp:1545` (timerCallback -> attachPad) with `Source/PluginProcessor.cpp:2533` (handleNoteOn sets selectedPad)  **Subsystem:** ui-wiring

**Failure walk (verifier-confirmed):** With a MIDI clip triggering pads
(notes 36-51; kbMode defaults false so :2530-2533 runs `selectedPad = pad`
for every note), the user grabs an inspector knob (LEVEL/PITCH/GRAIN...) and
drags. A foreign-pad note lands mid-drag; within one 15Hz tick (~67ms)
timerCallback (:1543-1547) sees `sel != attachedPad` and calls
attachPad(sel) unconditionally — no isBeingDragged/isMouseButtonDown guard
(contrast bpmLbl/playLbl, which ARE guarded by isBeingEdited() at
:1563/:1575). attachPad (:1460-1508) resets and rebuilds every
SliderAttachment; each ctor's sendInitialUpdate snaps the still-captured
slider to the NEW pad's value (visible jump), and the continuing drag —
anchored to valueOnMouseDown (JUCE Slider::mouseDrag) — now writes the NEW
pad's parameter via setValueAsPartOfGesture. The edit silently lands on the
wrong pad. Undo corollary: grain knobs' onDragStart (:234-238) pushed one
snapshot for the ORIGINAL pad; the continued mutation hits a different pad
with no matching onDragStart, so the coalesced undo entry no longer matches
the pad actually edited.

**Fix sketch:** In timerCallback, defer panel-follows-pad while any shared
inspector slider is being dragged: `if (sel != attachedPad && !editing)
attachPad(sel);` where `editing` = OR over the inspector sliders'
isMouseButtonDown() (or a flag set/cleared in onDragStart/onDragEnd).
attachedPad stays stale so the next tick after release reattaches —
panel correctly catches up. Mirrors the existing isBeingEdited() guards.

## 3. [MED] WAVE1 F1 regression: first MIDI tap of an unassigned pad is now silent
**Where:** `Source/PluginProcessor.cpp:2572` (handleAsyncUpdate) vs baseline `29969f7` assignPadCue (:563)  **Subsystem:** regression-diff

**Failure walk (verifier-confirmed against baseline):** At baseline,
handleNoteOn's unassigned branch called assignPadCue(pad), which set
`auditionPad = pad` (when !previewingA) plus lastTriggerPad /
++lastTriggerCount / ++uiDirty; processBlock consumes auditionPad ->
startVoice (:3920-3922 at HEAD, same ordering as baseline), so the
freshly-assigned pad PLAYED and the hero view followed. The WAVE1 F1 fix
(correctly moving pushUndo off the audio thread) routes the assign through
pendingAssignPos + triggerAsyncUpdate; handleAsyncUpdate (:2572-2584) runs
ONLY pushUndo/setCue/cueEnds — it never sets auditionPad, lastTriggerPad,
lastTriggerCount, or uiDirty. grep: `auditionPad =` has exactly one site
(:563, assignPadCue), and assignPadCue is now called only from the editor
drag-drop path (PluginEditor.h:2063). No other startVoice site covers
MIDI-tap-unassigned (:2527 kbMode, :2553 assigned, :3907/:3913 uiFifo).
**Result:** first tap silently drops a cue with no sound and no view-follow;
pad plays on the second tap. WAVE1_SPEC.md:104-106 accepted this as
"VISUAL-only — the triggering note never played audio under the current
behavior either" — that premise is FALSE against the baseline code.
Cue data is correct (self-healing), hence MED not HIGH. Note: Wave 1 was
Joe-validated, so the practical feel change may be acceptable — planner/Joe
ruling needed on restoring parity vs. ratifying the new behavior.

**Fix sketch:** In handleAsyncUpdate's per-pad assigned branch (after
setCue/cueEnds), restore parity: `lastTriggerPad = pad; ++lastTriggerCount;
++uiDirty; if (! previewingA.load()) auditionPad = pad;` (message thread;
audio thread consumes next block exactly as baseline). Re-run Joe's
tap-to-cue feel check afterward.

## 4. [MED] CMake POST_BUILD deploy breaks clean-machine builds
**Where:** `CMakeLists.txt:141-145`  **Subsystem:** build-packaging

**Failure walk (verifier-REPRODUCED both mechanisms in scratch tests):**
The unconditional POST_BUILD `cmake -E copy_if_different <vst3>
"C:/Program Files/Common Files/VST3/GentSampler.vst3/Contents/x86_64-win"`
assumes the destination exists and is writable (one-time ACL grant done only
on Joe's box, per its own comment at :137-140).
- **Mechanism 1** — destination missing/unwritable (any clean machine, no
  admin): copy_if_different exits 1 -> POST_BUILD fails the target ->
  build.bat:36 aborts with the generic "Build failed" BEFORE its own
  documented no-admin Documents\VST3 fallback (:48-53) can run. The
  README-advertised one-click build is dead on arrival for new users/CI.
- **Mechanism 2** — parent writable but leaf `x86_64-win` absent:
  copy_if_different treats the final component as a FILENAME, writing the
  VST3 bytes to a FILE named `x86_64-win` (exit 0) — malformed bundle,
  build reports success.
Not the deliberate FL-holds-the-plugin deploy gate (that case is documented;
this one isn't). Untouched by waves 1-3.

**Fix sketch:** Delete the POST_BUILD deploy (build.bat:47-56 already
installs with the admin/no-admin fallback) — or gate it
`if(EXISTS <dest>)` so it only fires on the pre-prepped dev box AND make
failure non-fatal, and copy into an ensured directory so the file-vs-dir
ambiguity can't produce a malformed bundle. Belongs naturally in the
packaging pass.

## 5. [MED] build.bat ships the ~2.6 GB CUDA/cuDNN pack into the user's VST3 folder
**Where:** `build.bat:47` (and the Documents fallback at :50)  **Subsystem:** build-packaging

**Failure walk (verifier-confirmed on disk):** build.bat:43 sets SRC to the
whole artefact bundle; :47 `xcopy /e /i /y` mirrors it recursively.
The artefact dir on this box currently holds all 13 CUDA/cuDNN DLLs
(~2.2-2.6 GB, dated Jun 25 — dev-box leftovers; POST_BUILD only copies the
2 small core ORT DLLs, CMakeLists.txt:116-121). Direct proof: the deployed
`C:/Program Files/Common Files/VST3/GentSampler.vst3/Contents/x86_64-win/`
ALREADY contains the full CUDA set — this path has already fired.
kEnableCuda is const false (StemSeparator.cpp:50) so none of it ever loads:
pure dead payload, contradicting the CPU-only small-install design
(CLAUDE.md architecture; first-run ModelDownloader fetch). BACKLOG.md's
packaging item frames the pack as "no code change implied" — incomplete:
build.bat actively re-installs whatever sits beside the artefact, and the
release will be cut on exactly this box.

**Fix sketch:** Exclude the pack at copy time — e.g. robocopy with
`/xf cublas*.dll cublasLt*.dll cudart*.dll cudnn*.dll cufft*.dll curand*.dll
nvrtc*.dll onnxruntime_providers_cuda.dll` (robocopy success = errorlevel
< 8, adjust the check), or delete the kCudaDlls names from the destination
after xcopy. Also correct the BACKLOG framing and have the packaging pass
clean the artefact dir at the source. Belongs in the packaging pass;
consider also cleaning the already-polluted deployed folder.

## 6. [LOW] doStemRenderJob resample branch OOB heap read on SR-mismatched stems
**Where:** `Source/PluginProcessor.cpp:2182`  **Subsystem:** persistence

**Failure walk (verifier-confirmed against JUCE source):** loadKitV2Audio
(:3464) and doStemCacheLoadJob (:3625) set StemSet::sampleRate from the stem
reader independently of the source; nothing validates or resamples at load.
If a hand-edited / cross-version .gentkit carries stems at a different SR
than source.flac, doStemRenderJob's branch at :2168 fires and calls the
4-arg `interp.process(ratio, stem.getReadPointer(c), atSrc.getWritePointer(c),
srcLen)` — JUCE's GenericInterpolator consumes ~ratio*srcLen input samples
with NO bounds check (its doc requires the input to contain that many), so a
stem shorter than ratio*srcLen is read past its end: OOB heap read on the
worker thread (garbage stem audio at best, AV at worst). The other two stem
consumers clamp (:1706-1716) or skip (:1082-1084); this is the sole
unclamped one. Unreachable from GentSampler-authored kits (stems always
encoded at source SR) — hence LOW, crafted/corrupted-file robustness.

**Fix sketch:** Use the safe 6-arg overload: `interp.process(ratio,
stem.getReadPointer(c), atSrc.getWritePointer(c), srcLen,
stem.getNumSamples(), 0);` — bounds every read, zero-fills past the end.
