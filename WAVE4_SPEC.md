# WAVE4_SPEC — FINAL (planner-owned; pending Joe approval)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `6af11aa`.
**Provenance:** drafted by SPEC-DRAFTER from PREPACKAGE_AUDIT_2.md (run `wf_90cc8154-5bd`, 6 verifier-confirmed findings); reviewed, corrected, and OWNED by the PLANNER 2026-07-06. F1=audit#1, F2=audit#2, F3=audit#3, F4=audit#4, F5=audit#5, F6=audit#6 (numbers are permanent IDs from PREPACKAGE_AUDIT_2.md).

## Planner rulings

- **OQ-A (F3) — RULING: path (a), restore audition parity, mandatory Joe feel re-check.** The Wave-1 deferral was accepted solely on the claim "the triggering note never played audio under the current behavior either" (WAVE1_SPEC.md:104-106) — verified FALSE against baseline `29969f7` (assignPadCue :559-563 set auditionPad; processBlock :3920 started the voice). A regression accepted on a refuted premise defaults to restoration; Joe validated Wave-1 feel without being told audition had been dropped, so his sign-off does not ratify the drop. Joe's hands-on check remains the final arbiter — the F3 fix carries an explicit stop-and-revert path if he prefers silent-first-tap. Under path (a), WAVE1_SPEC.md still gets a one-line correction annotation (the false premise must not mislead a third audit) — the draft only did this for path (b); corrected.
- **OQ-B (F4) — RULING: in-wave, and the GUARD option, NOT deletion.** The audit's "delete the POST_BUILD deploy, build.bat installs" sketch is wrong for this repo's workflow: the agents' gate (`build_test.sh`) runs `cmake --build` directly and never build.bat, so the POST_BUILD step is what refreshes the deployed VST3 on every gate run (deleting it resurrects the stale-binary landmine) and it IS the deliberate FL-holds-the-plugin gate (CMakeLists :138-140, CLAUDE.md landmine 2026-07-02). Fix = configure-time `IS_DIRECTORY` guard (also defeats Mechanism 2's file-named-`x86_64-win` residue, which bare `EXISTS` would not). Deploy failure on the dev box stays FATAL — that loudness is the FL gate; the draft's "make failure non-fatal" clause is rejected.
- **OQ-B (F5) — RULING: in-wave.** The release will be cut on exactly this box, the deployed folder is already polluted, and Joe may run build.bat any day between now and the packaging pass; the fix is a two-line batch edit that stops the bleeding now. Artefact-dir source cleanup + deployed-folder cleanup stay in the packaging pass. Not split into BULK+IMPLEMENTER (draft's suggestion) — the errorlevel/retry semantics ARE the fix; a two-line edit doesn't amortize a second dispatch.
- **OQ-C (F1) — RULING: IMPLEMENTER.** Nothing downstream catches a wrong guard placement (only guarding one publish, snapshotting after `separate()`, or clobbering the restore's own stemCacheKey on the bail path); the only checks are reviewer diff-read and a manual repro. Rule 1 forbids BULK here.
- **OQ-C (F6) — RULING: BULK.** This spec supplies the replacement line verbatim; the machine check is a whitespace-normalized template diff of the landed call against the spec's line (Rule 1 explicitly lists template diff as a machine catch) plus a `git diff --stat` scope check plus gates green. The silent-transposition risk the draft worried about is exactly what the byte-level template check eliminates. `MAX_BULK_RETRIES=1`, then promote to IMPLEMENTER per Rule 5.
- **F1 SCOPE EXTENSION (planner addition — the draft and the audit both missed the sibling hole):** the restoreGen guard alone does not cover a **direct load** mid-separation (drag song C in while A separates: `loadFile` is synchronous on the message thread, `adoptSourceBuffer` replaces the source object at :296 and clears stemSet/stemCacheKey at :327/:344 — but doStemJob's later publish at :1445 re-attaches A's stems to C; restoreGen never bumps on direct loads). The complete guard is `restoreGen changed OR source object identity changed`. The identity check closes the direct-load-before-publish case; direct-load-AFTER-publish is already safe (the :327 clear wipes the published set); restore-after-publish is documented DECISION-6 carry-forward. Two cheap conditions complete the whole matrix.
- **Draft factual correction (F5):** build.bat has exactly ONE errorlevel check (:48, after the Program Files copy); the Documents fallback at :50 is unchecked and prints `[OK]` unconditionally. The draft claimed two checks. Only the :48 check changes; the fallback stays unchecked (pre-existing behavior, accepted, noted below).

## Ground rules (Waves 1-3 protocol, unchanged)

- One fix = one CHECKPOINT COMMIT (`WAVE4 #N: ...`), PREPACKAGE_AUDIT_2.md status row updated in the same commit.
- Gates per checkpoint, from `.claude/agent.config`: `bash .claude/hooks/build_test.sh` (VERIFY_CMD) then `bash .claude/hooks/pluginval_gate.sh` (LINT_CMD).
- The deploy step failing because FL Studio holds GentSampler is a DELIBERATE gate: report and stop, close-FL-and-rebuild is Joe's action. **No agent ever terminates FL Studio or any user application, under any circumstances.**
- A pluginval "Open plugin (cold)" timeout is a REAL failure (2026-07-04 landmine): diagnose, never retry-loop, never dismiss as flake.
- After any aborted/killed dispatch: diff against the last checkpoint before continuing (2026-07-02 landmine).

## F1 — doStemJob: guard stemSet/stemCacheKey publish against restore AND source change (audit #1, order 1st)

**Where:** `Source/PluginProcessor.cpp` `doStemJob()` (:1299-1560).

**Fix:**
1. At entry, immediately after the existing `auto src = getSource();` null-check (:1301-1303), snapshot `const int genAtEntry = restoreGen.load();` (doStemCacheLoadJob :3594 pattern).
2. Define the bail condition, checked at TWO sites: `restoreGen.load() != genAtEntry || getSource() != src` (source identity: `SourceSample::Ptr` comparison against the entry snapshot — a direct load / kit load replaced the object at :296/:3442; a restore bumped the gen at :3673).
3. **Site 1 — before the stemSet publish (:1445-1448),** i.e. after `separate()` returns and the `set` is built: on bail, `delete set` (raw, never assigned — the :3639 precedent), `setStatus ("separation discarded (source changed during separation)")`, DBG line, `separating = false; stemProgress = 0.0f; ++uiDirty;`, `return`. Do NOT touch stemCacheKey (the incoming load/restore already set or cleared it for itself — clearing here would clobber it), do NOT set `wantStemRender`, skip the cache write and the proof-log WAV block.
4. **Site 2 — after the FLAC cache-encode loop, immediately before the stemCacheKey write/clear (:1493-1503).** The encode loop runs for seconds-to-minutes, so this is a real second window, and by then the stemSet from site 1 is ALREADY published — on bail this site must **retract** it: under `stemLock`, `if (stemSet == set) stemSet = nullptr;` then the same tail as site 1 (status, DBG, `separating=false`, `++uiDirty`, `return`) — and again do NOT write OR clear `stemCacheKey` (skip both the :1496 set and the :1502 clear), do NOT set `wantStemRender`. Leave the just-written cache dir on disk: it is keyed to A's content hash, benign, and correct if A ever returns (the cache README already declares the dir safe-to-delete).
5. No change to the cache-write internals (cacheOk / keyDir / encodeFlacTo / threadShouldExit poll) beyond the surrounding guard.

**AC:**
1. `genAtEntry` + `src` identity snapshots at entry; both bail sites present with the exact placement above (diff-read).
2. Site-1 bail: set deleted, no stemSet publish, no cache write, no stemCacheKey touch, no wantStemRender, `separating` cleared, uiDirty bumped (diff-read against the :3636 bail shape).
3. Site-2 bail: stemSet retracted under stemLock via pointer-identity check, stemCacheKey untouched in BOTH directions, no wantStemRender (diff-read).
4. Happy path byte-identical in behavior: no reordering of publish-then-cache (stems must still appear in the UI before the cache encode runs).
5. Gates green.
6. Joe repro (wave close, best-effort): start a separation on song A; before it finishes, load a previously saved GentSampler channel state / preset referencing song B into the SAME instance (FL channel-state load — reopening a whole FL project creates a new instance and won't exercise this), or simply drag a different song in mid-separation (the direct-load leg). Result: no trace of A's stems on the new source (STEMS view, padStemMask routing), stem status reads "separation discarded...". If the same-instance state-load repro proves impractical in FL, the drag-in-mid-separation repro alone suffices (it exercises the identity leg; the gen leg is reviewer diff-read).

**Not unit-testable** (live two-job worker race; same acceptance basis as Wave 1/2 #2/#9). **Tier: IMPLEMENTER.** Budget: 3/2.

## F2 — defer panel-follows-pad while an inspector control is mid-interaction (audit #2, order 2nd)

**Where:** `Source/PluginEditor.cpp` `timerCallback()` (:1545-1547). Trigger source `handleNoteOn` :2530-2533 is UNCHANGED.

**Fix:** gate the reattach: `if (sel != attachedPad && ! editing) attachPad (sel);` where `editing` is true if ANY control that `attachPad` (:1460-1508) rebinds is currently being interacted with:
- the 15 SliderAttachment-bound sliders (`padPitch, padLevel, padAtt, padRel, padCrush, padSpeed, padPan, padCutoff, padReso, padBleed, padGrainSize, padGrainDens, padGrainPos, padGrainSpray, padGrainPitch`) — `isMouseButtonDown()`;
- the 5 ButtonAttachment-bound buttons (`sliceStop, loopBtn, revBtn, grainBtn, freezeBtn`) — `isMouseButtonDown()` (same hijack class, tiny window, free to cover);
- the 3 ComboBoxAttachment-bound boxes (`playMode, chokeBox, ftypeBox`) — `isPopupActive()` (timers fire during the popup's modal loop; a rebind mid-popup lands the selection on the wrong pad).

Implementation shape is the implementer's call (an array of `Component*` + one loop is fine); no new flags/listeners unless `isMouseButtonDown()` provably misses a case — if a flag is needed, say why in the commit message. `attachedPad` stays stale during the interaction; reattach must fire on the FIRST tick where `editing` is false (no permanent skip — the condition is re-evaluated every tick, so this falls out naturally; implementer confirms). The `armedHandlePad` reset block (:1554-1559) and the bpmLbl/playLbl `isBeingEdited()` guards (:1563/:1575) are untouched.

**AC:**
1. Reviewer verifies 1:1 coverage: every attachment `attachPad` rebinds has its control in the `editing` OR-set (grep the attachment list :1486-1508 against the guard).
2. No behavior change when nothing is being interacted with (panel follows MIDI-selected pad within one tick, as today).
3. Gates green.
4. Joe repro (wave close): MIDI clip triggering pads 36-51 while dragging an inspector knob (e.g. GRAIN SIZE) on the selected pad — the whole drag lands on the pad it started on; panel visibly reattaches to the MIDI-driven pad only after mouse-up. Bonus check: same with a choke/mode dropdown held open.

**Not unit-testable** (JUCE mouse/timer interaction). **Tier: IMPLEMENTER** (which-controls-count and the sufficiency of `isMouseButtonDown()` are judgment). Budget: 3/2.

## F3 — restore audition-voice parity in handleAsyncUpdate (audit #3, order 3rd) — per OQ-A ruling: path (a)

**Where:** `Source/PluginProcessor.cpp` `handleAsyncUpdate()` (:2572-2584), baseline parity source `assignPadCue` (:559-563).

**Fix:** in the per-pad assigned branch, after `cueEnds[(size_t) pad] = kOpenSlice;` (:2583), add exactly:
```cpp
lastTriggerPad = pad;
++lastTriggerCount;
++uiDirty;
if (! previewingA.load())
    auditionPad = pad;
```
Notes the implementer must respect:
- Do NOT add `selectedPad = pad` — handleNoteOn already sets it on the audio thread (:2533); adding it here would be a duplicate, not parity.
- All four writes are atomics, message-thread, matching assignPadCue's own sequence; `processBlock`'s consumer (:3920 `auditionPad.exchange(-1)` → startVoice) is untouched.
- If several pads were assigned in one sweep, the last one wins the (single) auditionPad slot — accepted coalescing, same spirit as the sweep comment; note it in the code comment.
- Update the Wave-1 comment block (:2565-2571) to describe the restored parity instead of the superseded framing.
- Add a one-line correction annotation to WAVE1_SPEC.md next to :104-106: the "never played audio" premise was false (PREPACKAGE_AUDIT_2 #3); parity restored in WAVE4.

**AC:**
1. The four added lines match baseline `29969f7` assignPadCue's post-setCue sequence, minus `selectedPad` (reviewer diff-reads against baseline :559-563).
2. Audio-thread side of the Wave-1 fix untouched (atomic write + `triggerAsyncUpdate()` only; zero allocation).
3. Comment block + WAVE1_SPEC.md annotation updated per above.
4. Gates green.
5. **MANDATORY Joe re-check (this changes validated Wave-1 feel):** MIDI-tap an unassigned pad — it must audibly play on the FIRST tap, hero view follows. If Joe prefers the old silent-first-tap: STOP, revert this checkpoint, escalate to the planner for path (b) (ratify + document). His call is final.

**Not unit-testable** (audible/visual property on live MIDI). **Tier: IMPLEMENTER.** Budget: **MAX_IMPL_ATTEMPTS=2** / 2 — a four-line parity restore failing twice means the spec is wrong, not the implementer.

## F4 — CMake POST_BUILD deploy: configure-time IS_DIRECTORY guard (audit #4, order 4th) — per OQ-B ruling

**Where:** `CMakeLists.txt:141-145`.

**Fix:** wrap the deploy `add_custom_command` in a configure-time guard:
```cmake
if(IS_DIRECTORY "C:/Program Files/Common Files/VST3/GentSampler.vst3/Contents/x86_64-win")
    message(STATUS "GentSampler deploy step: ENABLED (dev-box destination present)")
    add_custom_command(TARGET GentSampler_VST3 POST_BUILD ...)   # existing :141-145 body, unchanged
else()
    message(STATUS "GentSampler deploy step: SKIPPED (destination absent — build.bat installs)")
endif()
```
- `IS_DIRECTORY`, not `EXISTS`: a leftover FILE named `x86_64-win` (Mechanism 2 residue) must select the SKIP branch.
- Deploy failure on the dev box remains FATAL — that is the deliberate FL-holds-the-plugin gate; do not soften it.
- Extend the existing :132-140 comment block with one line explaining the guard (clean machines: build.bat :47-56 is the sole install path).
- `build.bat` is untouched by this fix (F5 touches it separately).

**AC:**
1. Diff shows the guard wrapping the UNCHANGED custom command; both `message(STATUS ...)` branches present.
2. Positive branch: on this box, configure output shows `ENABLED`, gates green, deployed binary still refreshes on build.
3. Negative branch (machine check, since this box's destination exists and can't be renamed without elevation): a scratch `cmake -P` script in the session scratchpad exercising the same `if(IS_DIRECTORY ...)` on (a) a real directory, (b) a missing path, (c) a FILE named like the leaf — output pasted in the commit message showing dir→ENABLED, missing→SKIPPED, file→SKIPPED.
4. Gates green.

**Tier: IMPLEMENTER** (guard semantics + the fatal-on-dev-box invariant are judgment; the audit's own preferred option was rejected by the planner, so no latitude for reinterpretation). Budget: 3/2.

## F5 — build.bat: exclude the CUDA/cuDNN pack from the install copy (audit #5, order 5th) — per OQ-B ruling

**Where:** `build.bat:47` (Program Files line + its `:48` errorlevel check) and `:50` (Documents fallback line).

**Fix:** replace both `xcopy /e /i /y` calls with robocopy, with these NON-NEGOTIABLE semantics:
1. Exclusion: `/xf cublas*.dll cudart*.dll cudnn*.dll cufft*.dll curand*.dll nvrtc*.dll onnxruntime_providers_cuda.dll` — then the implementer lists the artefact bundle's actual DLL set (`dir /s /b *.dll` under `%SRC%`) and extends the list if any of the 13 CUDA-pack DLLs (audit F5) is not covered (e.g. `zlibwapi.dll` if present). `onnxruntime.dll` and `onnxruntime_providers_shared.dll` MUST still copy.
2. `/e` only — **NEVER `/mir` or `/purge`** (destructive to the ModelDownloader-parked pack and anything else beside the deployed binary; this is the COPY_PLUGIN_AFTER_BUILD landmine class, 2026-07-02).
3. `/r:0 /w:0` — robocopy's default is 1,000,000 retries at 30s each; an access-denied file on a no-admin box would otherwise hang the "one-click" build effectively forever.
4. Robocopy exit-code convention: success is `< 8`. The `:48` check becomes `if errorlevel 8 (` so a successful copy no longer trips the no-admin fallback. (There is exactly ONE existing check — the Documents fallback at :50 has none and prints `[OK]` unconditionally; leave that pre-existing behavior alone.)
5. Trailing-backslash footgun: robocopy arguments must NOT end in `\"` (the backslash escapes the closing quote) — drop xcopy's trailing backslashes from both destination paths.
6. Keep the `>nul 2>nul` / `>nul` redirects.

**AC:**
1. Both lines converted per items 1-6 (diff-read against each numbered semantic).
2. Before/after evidence in the commit message: `dir` of the artefact bundle's DLLs, then `dir` of a fresh Documents-fallback install destination showing the plugin binary + the two core ORT DLLs and ZERO excluded DLLs (the implementer can exercise the :50 line directly against a scratch destination — machine-checkable).
3. `kEnableCuda` / StemSeparator.cpp / CMakeLists untouched by this fix.
4. Gates green (build.bat itself isn't run by the gates; AC2 is the real check).

**Tier: IMPLEMENTER** (retry/exit-code/quoting semantics are the fix; a wrong threshold silently breaks the no-admin path). Budget: 3/2.

## F6 — bounds-safe 6-arg LagrangeInterpolator call in doStemRenderJob (audit #6, order 6th) — per OQ-C ruling: BULK

**Where:** `Source/PluginProcessor.cpp:2182` (the mismatched-SR `else` branch, :2172-2184).

**Fix:** replace the 4-arg call with EXACTLY this line (whitespace/wrapping per clang-format is the only permitted variation):
```cpp
interp.process (ratio, stem.getReadPointer (c), atSrc.getWritePointer (c), srcLen, stem.getNumSamples(), 0);
```
(JUCE `GenericInterpolator::process(speedRatio, in, out, numOutputSamplesToProduce, numInputSamplesAvailable, wrapAround)`; `wrapAround=0` zero-fills past the input's end. Argument order verified by the planner against JUCE 8.0.4.)

**AC (machine-checkable — the BULK justification):**
1. Whitespace-normalized grep of the landed call matches the spec's line token-for-token; the old 4-arg form is absent from the file.
2. `git diff --stat` for the checkpoint = exactly `Source/PluginProcessor.cpp` (one line changed) + `PREPACKAGE_AUDIT_2.md` (status row). Any other hunk = automatic FAIL.
3. The surrounding branch (`atSrc.setSize`, `atSrc.clear()`, `ratio`) and the matching-SR `if` branch (:2168-2171) are untouched.
4. Gates green (branch not exercised by any test — the checks above plus compile are the gate; acceptable for this crafted-file-robustness LOW).
5. Joe repro: not applicable (requires a hand-crafted SR-mismatched .gentkit that doesn't exist); the template diff stands as closing evidence.

**Tier: BULK** (template diff = machine check per Rule 1). `MAX_BULK_RETRIES=1`; on second failure promote to IMPLEMENTER per Rule 5.

## File plan

| File | Fixes |
|---|---|
| Source/PluginProcessor.cpp | F1, F3, F6 |
| Source/PluginEditor.cpp | F2 |
| CMakeLists.txt | F4 |
| build.bat | F5 |
| PREPACKAGE_AUDIT_2.md | status row per landed fix (same commit) |
| WAVE1_SPEC.md | F3: one-line correction annotation at :104-106 (false-premise note) |
| CLAUDE.md | wave close only (lead's commit): current state + any new landmine |

No other file may be touched. No new dependencies (robocopy is Windows-native; everything else uses facilities already in the file).

## Out of scope

- Waves 1-3 fixes (#7-#18) — do not re-touch.
- The refuted applyCueEdit finding — no action.
- The parked v2-classifier t-vs-activeT defect (AMENDMENT W3-A) — stays parked.
- Any `kEnableCuda` flip or ORT version change.
- Artefact-dir source cleanup and the already-polluted deployed folder → packaging pass (record as its first two line items).
- The DECISION-6 stems-carry-forward-on-restore design itself (F1 guards the in-flight publish only; completed-then-restored stem survival is documented, intentional behavior).
- No new doctest cases (no pure-function surface in any of the six; stated per-fix, not defaulted).

## Verification commands (every checkpoint)

```
bash .claude/hooks/build_test.sh        # VERIFY_CMD — Release build + ctest (116 cases)
bash .claude/hooks/pluginval_gate.sh    # LINT_CMD — strictness 5; FL-deploy-lock = report and stop
```
F4 additionally: the `cmake -P` branch-check script output (AC3). F5 additionally: the before/after `dir` evidence (AC2). Neither is exercised by the gate scripts on this provisioned box — the documented repro IS the gate for those two, performed by the implementer and re-checked by the reviewer.

## Tier assignment

F1, F2, F3, F4, F5 = IMPLEMENTER. F6 = BULK (machine check: token-exact template diff + diff-stat scope check + gates; justification under F6).

## Loop budget

Defaults from `.claude/agent.config`: MAX_IMPL_ATTEMPTS=3, MAX_REVIEW_CYCLES=2 for F1/F2/F4/F5. **F3: MAX_IMPL_ATTEMPTS=2** (second failure = spec problem, escalate). **F6: MAX_BULK_RETRIES=1**, then promote to IMPLEMENTER. Budget exhaustion anywhere escalates to the planner as a spec-sizing problem, never as a request for more attempts.

## Checkpoints (Rule 9 resume points)

One commit per fix, order **F1 → F2 → F3 → F4 → F5 → F6** (runtime MEDs first, build-packaging pair, LOW last). Each commit: code change + PREPACKAGE_AUDIT_2.md row (+ WAVE1_SPEC.md annotation in the F3 commit). Interrupted work resumes from `git log`/`git status` against this table — never by replaying a prompt; ambiguous partial state rolls back to the last checkpoint.

## Risks

- **F1:** the site-2 bail is the subtle one — it must RETRACT the already-published stemSet and must NOT touch stemCacheKey in either direction (a clear there clobbers the incoming restore's own key). Residual micro-windows between check and lock are accepted (same residual as the existing doStemCacheLoadJob pattern).
- **F2:** over-guarding is self-limiting (reattach re-evaluates every tick), but missing a control reintroduces the hijack for that control — hence the 1:1 coverage AC.
- **F3:** Joe may reject the restored feel; the revert path is a single checkpoint revert + planner escalation for path (b). Do not argue the case in the session — his call ends it.
- **F4:** the negative branch can't be exercised end-to-end on this box (destination exists, elevation required to remove); the scratch `cmake -P` check is the honest proxy. A future clean-machine/CI build in the packaging pass is the true end-to-end confirmation — note it there.
- **F5:** robocopy footguns (default infinite retries, trailing-backslash quote-escape, exit codes 1-7 being success) are each individually capable of breaking the one-click build while looking correct — all three are pinned by name in the ACs.
- **F6:** all six arguments are `int`-compatible, so a transposition compiles clean — the token-exact template check is load-bearing; the reviewer must not wave it through on "looks right".
- **General:** aborted dispatch → diff against the last checkpoint before continuing; never terminate FL Studio; a pluginval cold-open timeout is a finding, not noise.

## Joe's manual pass (wave close, final arbiter)

1. F1: drag a different song in mid-separation (and, if practical, load a saved channel state into the same instance) — no stale stems on the new source.
2. F2: MIDI clip across pads 36-51 while dragging an inspector knob — edit stays on the originating pad; panel catches up after release.
3. F3 (MANDATORY): first MIDI tap of an unassigned pad audibly plays + hero follows — confirm this is the feel you want; say so explicitly either way.
4. F4/F5 (best-effort): none required on this box beyond normal use; the packaging pass's clean-machine build is the real-world confirmation.
5. F6: none (template diff stands).

## Record

| Fix | Commit | Reviewer verdict (cycle 1) | Joe |
|---|---|---|---|
| F1 | `a333fc4` | PASS (nit: empty-key residual window — within accepted Risks-F1 residuals, characterized only) | PASS 2026-07-06 |
| F2 | `b12bc19` | PASS (1:1 coverage over all 23 rebinds verified) | PASS 2026-07-06 |
| F3 | `d2489cb` | PASS (nit: comment line-citation drift — lead-fixed at wave close) | **feel re-check PASS 2026-07-06 — restored first-tap audition ratified** |
| F4 | `c5fed96` | PASS (reviewer independently re-ran the 3-case cmake -P proof) | — |
| F5 | `bd7323c` | PASS (exclusion patterns independently verified against the real bundle) | — |
| F6 | `ea9a7f7` | PASS (token-exact template check, no transposition; BULK scope clean) | — |

**WAVE 4 CLOSED 2026-07-06: Joe manual pass GREEN** (post-reboot; the same-day
separation failure was diagnosed as box-state OOM — kernel paged-pool leak +
full C: pinning the pagefile — NOT a wave regression; separation confirmed
working after reboot).

Wave verdict: **PASS cycle 1, 0 blocking, 2 nits** (verdict.json, verdict_lint
exit 0, escalate=false). F1 checkpoint note: the original implementer dispatch
was killed by a session crash mid-gate; the lead recovered the on-disk diff,
reviewed it line-by-line per the aborted-dispatch landmine, and committed after
green gates — an orphaned link.exe from the killed run held the build artefact
(LNK1104) until a Joe-authorized kill of that PID cleared it.
