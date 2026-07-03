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
- Last completed: Phase D4 (2026-07-03) — the interaction rewire (blast-radius
  task). The dormant whole-band early-return hit-test in
  `WaveformView::mouseDown`/`mouseMove` (the pre-D4 `if (stemBandH > 0 &&
  e.y...) { ...unconditional return... }` blocks) is DELETED. Replaced with
  D1 §8's map: in STEMS view (painted view == `gent::sanitizeHeroView
  (heroView)==1` AND real lanes showing), a lane mute/solo x-zone test over
  the full `(0, waveBottom)` lane geometry claims a click ONLY within its own
  x-range (`gent::laneZoneAt` — new pure extraction, mute/solo boundary
  constants lifted verbatim from the dormant code, solo tested before mute to
  match its if/else-if precedence) at that lane's y (`gent::laneIndexAt`,
  from D3); everything else (including the wave-column middle of a lane)
  falls through unchanged to scrollbar/pan/flag-select/handle-drag/placeStart
  — same single edit path, zero duplicated drag/undo/snap logic. Flag-select
  hit-test now uses STEMS-correct pennant-row geometry (`[0, flagBarH)`,
  matching D3's `paintFlagsCuePlayheads(..., flagY=0, ...)` call) instead of
  composite's stale `flagBarY/flagBarH` row when in STEMS view; composite
  path unchanged. Hover (`hoverLane`) rewired to the same full-lane geometry,
  gated the same way; 30Hz timer untouched (hover is mouseMove-only state,
  no stomp). COMPOSITE view: zero lane hit-tests fire (`stemsReady` requires
  `heroReq==1`, always false in composite) — no dead zones where the old band
  was. New pure fn `gent::laneZoneAt(x,w,labW,soloW)` in EngineMath.h +
  6 new doctest cases in StemMaskTests.cpp (D4.1-D4.6: boundary pixels both
  zones, degenerate-small-w precedence, reference-impl sweep, laneIndexAt x
  laneZoneAt composition over a scripted click grid) — riding the existing
  ctest target, no CMakeLists change. Full gate green (build + 45 ctest
  cases/55,860 assertions + pluginval strictness 5). Prior: D3 (STEMS lanes
  paint + composite band retirement), D2c (heroView state/persistence/seg +
  STEMS placeholder), D1 doc ratified + addendum, D2a/D2b
  (resolveHeroView/sanitizeHeroView); TEST_TARGET_TASK.md and Phase C6 both
  complete 2026-07-02/03. NOTE: build/ is a JUNCTION to D:\GentSamplerBuild
  (C: was 100% full) — all paths unchanged, bits live on D:.
- In progress: Phase D at the D6 GATE (2026-07-03). D5 committed (lifecycle
  matrix + DECISION-6 stemSet clear, 54 ctest cases); R3 PASS (single-edit-
  path CONFIRMED, fence CLEAN); D6 captures delivered to Desktop
  (sbsD_stems/sbsD_composite/D6_placeholder/D6_midsep/floor shots) — all
  functional observations confirmed live during capture (mute/solo/flag-
  select/CTA-inert/no dead zones). Seg "COMPOS…" truncation found in
  captures and FIXED (text-sized segments via HeroViewSeg::preferredWidth).
  HOLDING for Joe: (a) FL validation of the phase, (b) ruling on stemStatus
  staleness (stale narration text survives new-file load — one-line fix in
  the DECISION-6 block awaits authorization, or backlog).
- Next up: Joe's FL pass closes Phase D; then BACKLOG items (standalone ORT
  DLLs, extend-undo, P3 breathing retry).
- Blocked on: host-process CUDA integration fault (see GPU_HANDOFF.md §3).

## Conventions
- Language/stack: C++17, JUCE 8.0.4 (FetchContent), CMake ≥3.22,
  MSVC (VS 2022 Build Tools), x64, Windows. MSVC `/utf-8` enforced.
- Style: Formatter: clang-format (config in .clang-format at repo root). Match existing code style in Source/.
- Tests: ctest unit tests (tests/, doctest, logic-only — pure functions in
  Source/EngineMath.h). Run: `ctest --test-dir build -C Release --output-on-failure`.
  gate.sh runs them as GATE 2, after build, before pluginval.
- Verification command: `cmake --build build --config Release --parallel`
  (gate.sh auto-configures with `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` if build/ is missing,
  then `ctest --test-dir build -C Release --output-on-failure`, then runs pluginval at strictness 5 if it's on PATH)

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
Follow ROUTING.md in the repo root. Summary:
- Fable plans and gates. Sonnet implements. Haiku only does machine-verifiable work.
- Route by verifiability: if nothing automatic catches a failure, minimum Sonnet.
- Two failures at any tier → escalate with failure context attached.

## Definition of done
- [ ] Verification command passes
- [ ] pluginval passes (when installed) — strictness 5
- [ ] Acceptance criteria from the spec checked off
- [ ] No files touched outside the spec
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
- 2026-07-02 — A killed or failed agent dispatch may leave debris in the
  working tree (one killed fix-agent was mid-replacing a paint block with
  debug scaffolding). After ANY aborted dispatch: diff against the last
  green commit before continuing; recover by checkout + reapplying only
  the reviewed hunks.
- 2026-07-02 — The standalone PERSISTS the last-loaded file, and slice-export
  artifacts (e.g. GentSampler_Pad12.wav) can restore as a silent EMPTY source
  — the wave renders blank and looks like a paint bug. Check the filename
  label in the hero before diagnosing render bugs.
- 2026-07-02 — Undo scope is PARTIAL: CueSnap (PluginProcessor.cpp ~325) snapshots
  cue/end windows ONLY — stem-source (padStemMask) and grain param changes are NOT
  undoable; Ctrl+Z after those reverts just the slice windows. Both surfaces sync
  correctly to whatever undo restores. Backlog: extend undo coverage (BACKLOG.md).
  Don't "fix" this casually — undo granularity needs a spec.
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
- Unicode in UI strings requires MSVC `/utf-8` (already set) — don't remove it
  or middots/arrows mojibake.
