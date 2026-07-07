# CLAUDE.md — GentSampler
# This file is loaded automatically every session. It is the project's memory.
# Keep it under ~150 lines. If it grows past that, move detail into /docs and link it.

## What this project is
GentSampler is a 16-pad flip sampler VST3 (+ Standalone) for Windows, built on
JUCE 8. Drop a track in → auto BPM/key detection → transient cue points →
tempo-synced time-stretch (Signalsmith Stretch) → play/flip on 16 pads → get
work OUT (drag slices as WAV, performances as MIDI, kit export, clearance-ready
Flip Log). v2 work in progress: AI stem separation via ONNX Runtime
(htdemucs_6s) and Basic Pitch audio-to-MIDI transcription. "Shipped" = builds
clean, passes pluginval, and runs stable inside FL Studio.

## Current state (inferred from GENTSAMPLER_AUDIT.md 2026-06-28 and GPU_HANDOFF.md 2026-06-25 — correct me)
- Last completed: PHASE D COMPLETE — Joe FL-validated 2026-07-03; the full
  face matches the mockup (COMPOSITE⇄STEMS stem map, docs/STEM_VIEW_MODEL.md
  RATIFIED + 2 addenda). NOTE: build/ is a JUNCTION to D:\GentSamplerBuild
  (C: was 100% full) — all paths unchanged, bits live on D:.
- In progress: PHASE 3 (PHASE3_SPEC.md, executing Downloads/PHASE3_TASK.md).
  Part 0 bench cleanup: 0.1 DONE, 0.2 DONE, 0.3 DONE — CueSnap now covers
  cue/end + padStemMask + all 7 grain params (write-if-changed, message-
  thread APVTS writes); AMENDMENT 0.3-A also fixed a pre-existing undo/redo
  slot-arithmetic bug (chained tracked edits used to skip a step on undo) —
  see the rewritten landmine below. Build/ctest(54)/pluginval green.
  0.4 (optional breathing-wave retry) SKIPPED 2026-07-03 — isolation harness
  ran, mechanism UNCONFIRMED (contradicted: all 4 rig variants agreed within
  0.08 luminance on a native-peer harness). Zero production diff. See BACKLOG.md
  for the full retry writeup.
  P1 feature cache DONE (P1a/b/c: FrameFeatures in the existing FFT pass +
  aggregateSliceFeatures, 8MB-cap). P2 classifier DONE (P2a tests / P2b BULK
  body / wiring): gent::classifySlice + kThreshStems/kThreshNoStems table,
  worker-thread doClassifyJob writes a per-slice report to Documents\
  GentSampler, dev "Classify slices -> report" item in the SLICE menu. R1
  PASS. 77 ctest cases.
- **RE-SCOPED 2026-07-04:** PHASE3_SPEC.md is SUPERSEDED FOR v1 (classifier +
  its ear-gate parked to v2; that gate is DISSOLVED for v1). v1 executes
  SECTIONS_SPEC.md (from Downloads/PHASE3_RESCOPE.md + PHASE3_TASK_SECTIONS.md):
  autoslice = mode menu of intents, SECTIONS primary. Part 1 BARS: DONE
  (`6253f65`, doctest 94), Joe FL-validated 2026-07-04. Part 2 NOVELTY: DONE
  (`a836446`, doctest 104, gate+deploy green) — spectral-change boundaries
  from the P1 FrameFeatures cache; dev submenu "Sections novelty (dev)"
  (61-63 Report / 64-66 APPLY @ few/medium/many); report file =
  Documents\GentSampler\GentSampler_sections_report.txt.
  NOVELTY EAR GATE: **APPROVED** by Joe 2026-07-04 ("works well"). Part 3
  SLICE split-chip: DONE (`37b6c06`), Joe-validated 2026-07-05 —
  **SECTIONS task COMPLETE** (record in SECTIONS_SPEC.md).
- In progress: **KIT v1** (KIT_SPEC.md). Part A hit isolation: DONE
  (`93b88c4`, doctest 110), Joe FL-validated 2026-07-05 ("working as
  intended"). Part B portable .gentkit v2: DONE (`cd44957`, gate+deploy
  green) — kit = layout + AUDIO: ZIP(kit.xml + source.flac 24-bit + stems if
  present), worker-thread save (requestKitSave/doKitSaveJob, shared
  buildKitStateXml so v1/v2 XML never drift), PK-sniff loader with v1
  back-compat, adoptSourceBuffer extraction. LEAD REVIEW FIX: loadFile also
  PK-sniffs — a host-project restore whose stored path IS a .gentkit adopts
  the kit's AUDIO only (project state stays in charge); without this the
  project reopened with a silent empty source (Pad12-ghost family).
  Parts B+C FL-validated 2026-07-05 — **KIT v1 COMPLETE** (isolation +
  portable .gentkit v2 + stem cache; records in KIT_SPEC.md). With SECTIONS
  done, the re-scoped Phase 3 v1 headline tasks are both shipped.
- DATA INTEGRITY: DONE (`2ee2f53`, gate green, review clean) — both BACKLOG
  HIGH items resolved: restoreGen restore-authority guard (in-flight analysis
  can no longer clobber restored cues; restore cancels queued derive-intents)
  + async validated restore decode (zero message-thread decoding on project
  reopen; keepCues protects restored cues from the late decode's default-cue
  loop; tempDir-export paths refused; empty decodes never adopted).
  Joe-validated 2026-07-05 ("everything seems to be good"). ADDENDUM T
  teardown hardening: DONE (`b088fa0`) — Joe's crash-on-close diagnosed via
  Event Log (pre-07-04 = the already-fixed CUDA-preload teardown fault;
  remaining signature = worker force-killed by stopThread(3000) mid-FLAC-
  encode/mid-separation → heap corruption). All long worker jobs now
  abortable (chunked encodes/reads polling threadShouldExit; StemSeparator
  shouldAbort per segment + per bag submodel, abort = existing failure
  shape, partials never adopted); dtor stopThread(10000) headroom. AWAITING
  Joe's close-during-separation / close-during-cache-write check. AUDIT run 2026-07-05
  (multi-agent, wf_81cf0f51-134): 18 confirmed findings -> PREPACKAGE_AUDIT.md.
  **WAVE 1 (six HIGHs) COMPLETE** per the full ROUTING pipeline: drafter ->
  planner spec (WAVE1_SPEC.md, Joe-approved) -> implementer checkpoint commits
  e12235e/d428a67/6436a41/647636b/9265c9a/978d376 (+W1-A: implementer caught a
  spec gap and stopped per protocol) -> Opus reviewer PASS (verdict.json,
  verdict_lint, 0 blocking) -> lead applied 1 nit (adf138e). Wave 1
  Joe-validated 2026-07-05. **WAVE 2 (nine MEDs) COMPLETE** (WAVE2_SPEC.md,
  Joe-approved): checkpoints ad75ec8..67f927a + correction 1a29d26 — the
  Opus reviewer FAILED cycle 1 on a real catch (signed graveyard ring
  indices -> INT_MAX UB -> OOB heap write; the spec's own overflow-horizon
  check), lead applied the specified fix, cycle-2 PASS 0 findings. Wave 2
  Joe-validated 2026-07-05. **WAVE 3 (three LOWs) COMPLETE** (WAVE3_SPEC.md,
  Joe-approved; planner-authored, drafter skipped per Rule 8): 49c9fd3 (#17
  decode-cap uniformity) / 1e8f7ef (#16 first-heartbeat dir create) /
  c624dec (#18 four 1c-fallthrough doctests, 110->116). #18's test-writing
  PROVED the spec's demotion case unreachable and surfaced a latent
  t-vs-activeT defect in the PARKED v2 classifier — planner ruling: deferred
  to the v2 re-gate (AMENDMENT W3-A + BACKLOG entry; reviewer independently
  confirmed the proof). Reviewer PASS cycle 1, 0 findings.
  **AUDIT BURN-DOWN COMPLETE: 18/18 findings resolved** (17 fixed +
  reviewer-verified across three waves; 1 characterized + deferred to v2
  with proof). Wave-3 two-repro smoke Joe-PASSED 2026-07-06. AUDIT 2 run
  2026-07-06 (multi-agent, wf_90cc8154-5bd, repo @ 6af11aa): 10 lanes incl.
  new regression-diff + build-packaging lanes; 7 raw -> 6 CONFIRMED
  (0 HIGH / 5 MED / 1 LOW), 1 refuted -> PREPACKAGE_AUDIT_2.md. Six lanes
  came back clean (wave 1-3 hardening held). Notables: #3 = WAVE1 F1 fix
  regressed first-tap audition on unassigned pads (spec premise was false;
  needs Joe ruling), #4/#5 = packaging blockers (POST_BUILD deploy breaks
  clean-machine builds; build.bat xcopy ships the 2.6 GB CUDA pack — the
  deployed folder on this box already has it). **WAVE 4 (all six) COMPLETE
  pending Joe's manual pass** (WAVE4_SPEC.md, planner-owned, Joe-approved):
  checkpoints a333fc4 (F1 doStemJob gen+identity publish guard — planner
  extended scope to cover direct-load mid-separation, not just restore) /
  b12bc19 (F2 panel-follows-pad deferred while any of the 23 inspector
  controls is mid-interaction) / d2489cb (F3 audition parity restored per
  OQ-A path a — WAVE1's "VISUAL-only" premise was FALSE, annotated in
  WAVE1_SPEC.md; **Joe's feel re-check MANDATORY**, revert+path-b if
  rejected) / c5fed96 (F4 IS_DIRECTORY configure guard on the deploy step;
  dev-box failure stays FATAL = the FL gate) / bd7323c (F5 build.bat
  robocopy /xf CUDA exclusion, /r:0 /w:0, errorlevel 8) / ea9a7f7 (F6
  6-arg LagrangeInterpolator, BULK dispatch, token-exact template check).
  Opus reviewer PASS cycle 1, 0 blocking, 2 nits (verdict_lint 0); lead
  applied nit 2 at wave close. **Joe manual pass GREEN 2026-07-06 (WAVE 4
  CLOSED; F3 restored first-tap audition ratified).** Same-day separation
  failure = box-state OOM (paged-pool leak + full C:), fixed by reboot —
  NOT a wave regression (memory: disk-and-oom-recovery). ~5.2 GB C: cleanup
  plan CANCELLED 2026-07-06 (Joe cleared ~10 GB himself). **PREPACK_UX
  COMPLETE — Joe manual pass GREEN 2026-07-06 (both U1+U2 PASS, task
  CLOSED)**; details:
  (PREPACK_UX_SPEC.md, planner-owned, Joe-approved): U1 `2b1ce4b` (hero
  zoom held across same-pad re-triggers via new lastAssignCount atomic;
  snap only on fresh assign or different pad, OQ-1 ruling (a); FOLLOW
  contract unchanged) + U2 `083ad25` (open-slice END handle anchors at the
  DRAWN grip, not len-1 — handleDragBegin openEndAnchor param, 3 call
  sites, isOpenSlice-gated so non-open/cue/grain byte-identical). Opus
  reviewer PASS cycle 1, 0 findings, gates re-run independently (116
  ctest). U1's gate was FL-lock-blocked ~40 min (deliberate gate; lead
  resumed after release). **PHASE E POLISH PASS IN PROGRESS** (doc =
  PHASE_E_POLISH_PASS.md, repo root; rules: E1→E6 one at a time, audit-first
  per task, full clean files, verify 1040×700 + 880×592 floor with
  before/after captures — rig = scratchpad capture.ps1; build with
  `cmake --build build --config Release --parallel`; capture BEFORE
  implementing for the before shots; kill only OUR launched standalone,
  never FL). STATUS: **E1 DONE + Joe GREEN** (5ff1149; mockup gate passed
  via E1_HEADER_MOCKUP.html, variants B/B: MIDI cluster keeps ⠿MID drag-out,
  undo/redo→FILE zone; tagline kerning .22→.14 to fit un-ellipsized —
  flagged, accepted). **E2 DONE + Joe GREEN** (d71a88a; gent::fmt in
  EngineMath.h + tests/FormatTests.cpp doctests, all 15 attachPad sliders +
  header labels routed, LNF createSliderTextBox → Theme::mono tabular).
  **E3 DONE pending Joe** (4cffcbe; rainbow padColour DELETED — pads 13-16
  magenta source; padSourceColour→neutral bone fallback, single-stem pads
  keep stem tokens; playing = amber face + pulsing ring; .chip.on demoted
  to outline+text, TrigPad/HeroViewSeg actives promoted to SOLID+ink).
  NEXT: E4 (hero strip: STEMS READY badge, remove CUE tag, middle-ellipsize
  filename, 0:00 ruler anchor) → E5 (chip taxonomy; QUANTIZE toggle-vs-action
  is a REPORT-AND-WAIT item; E5.2/5.3 may need quick mockup) → E6 sweep.
  Packaging pass queued AFTER Phase E.
- Blocked on: host-process CUDA integration fault (see GPU_HANDOFF.md §3).

## Conventions
- Language/stack: C++17, JUCE 8.0.4 (FetchContent), CMake ≥3.22,
  MSVC (VS 2022 Build Tools), x64, Windows. MSVC `/utf-8` enforced.
- Style: Formatter: clang-format (config in .clang-format at repo root). Match existing code style in Source/.
- Tests: ctest unit tests (tests/, doctest, logic-only — pure functions in
  Source/EngineMath.h). Run: `ctest --test-dir build -C Release --output-on-failure`.
  The primary gate runs them after the build (see below).
- Verification commands and loop budgets: defined ONCE in `.claude/agent.config`
  (gate.sh sources it; env vars override per session — ROUTING.md Rule 10).
  Primary = `.claude/hooks/build_test.sh` (auto-configure + Release build + ctest);
  secondary = `.claude/hooks/pluginval_gate.sh` (pluginval strictness 5,
  auto-skips if not installed, NO retry — see the 2026-07-04 landmine).

## Architecture (short)
- Plugin targets: VST3 + Standalone, `IS_SYNTH`, MIDI in, 16 optional per-pad
  stereo output buses. `VST3_AUTO_MANIFEST FALSE` — do not re-enable (skips a
  build-time LoadLibrary step that breaks the one-click build).
- Time-stretch: Signalsmith Stretch (MIT), fetched from `main` — if its API
  drifts, pin a tag in CMakeLists.txt. Master pitch/tempo = true stretch;
  per-pad pitch = classic repitch (by design, MPC-style).
- ONNX Runtime pinned at **1.18.1** (CUDA 11.8/cuDNN 8.9 legacy conv path).
  1.20+ requires the cuDNN-9 frontend which cannot build htdemucs's first conv
  on this GPU; 1.21/1.22 add a CUDA stream regression. Do NOT upgrade ORT
  without reading GPU_HANDOFF.md §2.
- ORT is loaded manually at runtime (no onnxruntime.lib link) so the plugin
  module has zero load-time ORT dependency; only the two small core DLLs are
  copied next to the VST3. CUDA EP + cuDNN are a first-run download
  (ModelDownloader) to keep CPU-only installs small.
- Audio thread reads sample state under SpinLock; 32-voice pool; tempo changes
  re-render in the background. Don't allocate or lock-contend in processBlock.

## Routing
Follow ROUTING.md (v4) in the repo root. Summary: PLANNER (Fable) owns specs +
arbitrates escalations; SPEC-DRAFTER (Sonnet) drafts cheaply so the planner only
corrects; IMPLEMENTER (Sonnet) does judgment work; REVIEWER (Opus, senior)
reviews then escalates hard calls to PLANNER; BULK (Haiku) only takes
machine-verifiable work. Route by verifiability. Two human touchpoints: approve
the spec, accept the result. Loop budgets (`.claude/agent.config`) bound every
autonomous run; failed loops resume from git state, never replay (Rule 9);
reviewer verdicts are validated JSON (`.claude/state/verdict.json`).

## Definition of done
- [ ] Verification gates pass (loop 3 green: build + ctest, then pluginval
      strictness 5 when installed — commands in `.claude/agent.config`)
- [ ] Acceptance criteria from the spec checked off
- [ ] No files touched outside the spec
- [ ] For interactive work: Joe's hands-on FL Studio pass complete
- [ ] CLAUDE.md "Current state" section updated

## Do not
- Do not add dependencies without spec authorization.
- Do not refactor beyond the task's declared scope.
- Do not guess on ambiguous specs — stop and ask.
- Do not upgrade ONNX Runtime past 1.18.1 or flip `kEnableCuda` to true
  without a spec that cites GPU_HANDOFF.md.
- Do not touch build/ by hand — it is generated.

## Known landmines
Running list of past failures and their fixes, so they never recur.
- 2026-07-03 — P3 breathing-wave isolation (task 0.4, SKIPPED): a scratch
  JUCE harness reusing `build/_deps/juce-src` needs a SHORT build path —
  MSVC's FileTracker fails (`FTK1011`) under the default deep Temp scratchpad
  path. Also, off-screen `Image(Image::ARGB,...)` silently no-ops all
  drawing outside a real component/peer (routes through a native/Direct2D
  image type with no device context) — use `SoftwareImageType()` explicitly,
  or better, a real `Component` + `createComponentSnapshot()` for anything
  meant to be a faithful proxy of `paint()`. Full retry numbers in BACKLOG.md.
- 2026-07-02 — A killed or failed agent dispatch may leave debris in the
  working tree (one killed fix-agent was mid-replacing a paint block with
  debug scaffolding). After ANY aborted dispatch: diff against the last
  green commit before continuing; recover by checkout + reapplying only
  the reviewed hunks.
- 2026-07-04 — An implementer agent FORCE-KILLED FL Studio (taskkill on
  FL64.exe) to clear the deploy-step file lock, directly against its
  instructions to report-and-stop; the kill happened to fail, but it could
  have destroyed Joe's unsaved project. RULE: no agent ever terminates a
  user application — the deploy lock while FL holds GentSampler is a
  DELIBERATE gate (close FL and rebuild), never a fault to "fix". Every
  dispatch prompt that runs gate.sh must state this; the lead reviews any
  such dispatch's diff line-by-line before trusting it (this one was clean
  and scoped, verified independently).
- 2026-07-02 — The standalone PERSISTS the last-loaded file, and slice-export
  artifacts (e.g. GentSampler_Pad12.wav) can restore as a silent EMPTY source
  — the wave renders blank and looks like a paint bug. Check the filename
  label in the hero before diagnosing render bugs.
- 2026-07-03 — Undo scope (rewritten, was "PARTIAL" 2026-07-02, now extended
  by Phase 3 task 0.3): CueSnap (PluginProcessor.cpp, CueSnap/snapshot/
  applySnap) now covers cue/end + padStemMask + all 7 per-pad grain params
  (grainOn/Size/Dens/Pos/Freeze/Spray/Pitch). Still NOT covered: per-pad
  pitch/gain/pan/choke/play-mode/speed and any global param. Host automation
  actively driving a grain param will overwrite what undo just wrote on its
  next pass (accepted, D-0.3c) — undo does not fight automation, same as any
  UI write. Grain restore goes through apvts.getParameterAsValue() (message
  thread only, write-if-changed).
- 2026-07-03 — AMENDMENT 0.3-A, two undo-stack bugs found+fixed inside 0.3
  (both pre-existing, not caused by the 0.3 diff, first exposed by 0.3's own
  chained-edit tests): (1) pushUndo()'s "seed on empty history" pushed two
  entries but every later call pushed only one with no fix-up of the previous
  slot, so undo() silently skipped the most-recent-but-one edit on any 2+-
  edit chain (repro'd with plain SOURCE-chip clicks, no grain code involved)
  — fixed by having pushUndo() fix up history[undoPos] with a fresh snapshot
  before pushing the new placeholder (undo()/redo() were already correct;
  see the slot-diagram comment above pushUndo() in PluginProcessor.cpp).
  (2) Restoring grainOn/grainFreeze via applySnap() synchronously re-fires
  every Button::Listener on that toggle (JUCE's ButtonAttachment ->
  setToggleState(..., sendNotificationSync) -> sendClickMessage()), which
  re-triggered the editor's own undo-push listener FROM INSIDE undo()/redo()
  — fixed with a restoringSnap guard (GentSamplerAudioProcessor::
  isRestoringSnap()) that the editor's GrainTogglePush checks before pushing.
  Verified: edit-A/edit-B/undo/undo/redo/redo round-trips correctly for both
  SOURCE-chip and grain-knob chains; chained SMART-slice-then-SMART-slice
  undo lands on the first slicing's result, not two back.
- 2026-07-02 — COPY_PLUGIN_AFTER_BUILD deleted the deployed plugin: JUCE's
  copyDir.cmake REMOVE_RECURSEs the destination, wiping the CUDA/cuDNN DLLs
  next to the deployed binary (restored from the artefact dir). Deploy is now
  a custom merge-only copy_if_different post-build step (CMakeLists end) —
  never re-enable the JUCE flag. Deploy fails (on purpose) if FL Studio has
  GentSampler loaded: close FL and rebuild. One-time ACL grant on the
  deployed folder via Desktop "Fix GentSampler VST3 Access.bat".
- 2026-06 — ORT 1.22 + cuDNN 9: conv frontend cannot build htdemucs's 343,980-wide
  1-D conv; 1.21/1.22 CUDA stream regression errors at Add nodes → pinned 1.18.1.
- 2026-06 — `CUDA_MODULE_LOADING=EAGER` regressed the conv on the 6 GB card → reverted.
- 2026-06 — GPU inference crashes FL Studio's process (works standalone) →
  CUDA gated off; the thread/context + `cudaSetDevice(0)` fix is ALREADY
  implemented and did not fix it — don't re-try it as step 1.
- 2026-07-04 — Cold-open hang: `ensureOrtLoaded()` (StemSeparator.cpp) preloaded
  the ~2.6 GB CUDA/cuDNN pack via `LoadLibraryExW` on the worker thread the
  ctor starts, ON THE CONSTRUCT PATH. CUDA DllMain init blocks/faults inside a
  host process, so plugin construction stalled indefinitely — pluginval "Open
  plugin (cold)" timed out at 30s (3/3) and the stem-engine-check log was never
  written. Fixed: the whole preload (+ `g_cudaSetDevice` resolve) is now gated
  behind `if (kEnableCuda)` (const false) so construct does ZERO CUDA DLL
  loading; the CPU `onnxruntime.dll` load + `Ort::InitApi` are independent and
  untouched. Pre-existing since the initial commit — dead GPU groundwork that
  should have been gated when CUDA was shelved; NOT a Phase 3 change.
  MECHANISM (why it hid): cold vs warm OS file cache. The first cold load of
  2.6 GB is slow/hangs; once Windows has the DLLs cached (e.g. right after a
  standalone/FL/stem session) the same load finishes under 30s and pluginval
  "passes." **The documented "transient pluginval flake" that gate.sh's
  retry-once was built around WAS THIS EXACT HANG hitting warm cache** — it was
  never a flake. DO NOT re-add a retry to paper over a cold-open stall, and DO
  NOT dismiss an "Open plugin (cold)" timeout as noise: it means real blocking
  work is on the construct path (CUDA/model/ORT probe, or a synchronous file
  load — see the applyStateTree restore item in BACKLOG.md). Diagnose it; the
  engine-check log NOT updating after a run is the tell that the probe hung.
- Unicode in UI strings requires MSVC `/utf-8` (already set) — don't remove it
  or middots/arrows mojibake.
