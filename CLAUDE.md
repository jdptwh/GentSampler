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
- Last completed: SLICE_FEEL_TASK.md Task F4 — arrow-key nudge + active-handle
  affordance. `WaveformView`/`SliceDetailStrip` both call
  `setWantsKeyboardFocus(true)` so clicking either grabs plugin keyboard focus;
  unhandled keys bubble via JUCE's normal focused-component -> parent-chain
  KeyListener walk to the editor's single `keyPressed` (non-ctrl branch),
  confirmed the Ctrl+Z/Shift+Z/Y branch is untouched and still returns early
  (AC-F4.7). Bindings: Left/Right/`,`/`.` nudge the armed handle (Shift = fine
  rate); Up arms CUE, Down arms END. Armed-handle state is editor-held
  (`armedHandle`/`armedHandlePad`), defaults to CUE, resets to CUE whenever
  `selectedPad` changes (checked every `timerCallback` tick), and is also set
  the moment either surface's `mouseDown` arms a CUE/END gesture via a new
  `onHandleGrabbed` callback fired right after `handleDragBegin` at all 6 call
  sites (4 map, 2 strip) — mirrored into `SliceDetailStrip::setArmedHandle` for
  paint. Increment = 1 strip-pixel (`SliceDetailStrip::stripSpp()`, a new
  public accessor exposing `(zoomHi-zoomLo)/(waveR-waveL)`); Shift =
  `max(1, round(stripSpp/10))`. Nudge applies through the SAME edit calls the
  drag path uses — CUE via `setCue(pad, s, false)`, END via
  `applyEndHandleDrag(p, pad, s, 8*stripSpp)` — never through `resolveSnap`
  (nudge is spec'd as snap's escape hatch); one shared `nudgeHandle()` helper
  in PluginEditor.cpp, no third edit implementation. Undo coalescing: a
  600ms-since-last-nudge timestamp (`juce::Time::getMillisecondCounter()`,
  matching the codebase's existing lazy-timestamp idiom) gates `pushUndo()` so
  a rapid burst is one undo entry. Affordance: strip-only, armed cap keeps its
  existing full-alpha `Theme::accent` fill and gains a 1px accent outline ring
  (`g.strokePath` on the same triangle `Path`); the other cap is byte-
  unchanged; map handle rendering untouched. Build clean, pluginval strictness
  5 SUCCESS; only Source/PluginEditor.h/.cpp touched (PluginProcessor
  untouched — F4 needed no processor change). F5 (grain marker consistency,
  reusing the F1 engine, no snap, no nudge) is next and last.
- In progress: Nothing live — F4 built+gated, holding for F5.
- Next up: SLICE_FEEL_TASK.md Task F5 — grain-marker relative-drag + Shift
  fine mode via the same `HandleDragEngine` accumulator (no snap, no arrow
  nudge); reviewer gate after F5 (full diff, single-edit-path invariant).
- Blocked on: host-process CUDA integration fault (see GPU_HANDOFF.md §3).

## Conventions
- Language/stack: C++17, JUCE 8.0.4 (FetchContent), CMake ≥3.22,
  MSVC (VS 2022 Build Tools), x64, Windows. MSVC `/utf-8` enforced.
- Style: Formatter: clang-format (config in .clang-format at repo root). Match existing code style in Source/.
- Tests: none — the build is the machine gate; pluginval is the second gate
  when installed. The reviewer agent is the only check on logic until tests exist.
- Verification command: `cmake --build build --config Release --parallel`
  (gate.sh auto-configures with `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` if build/ is missing, then runs pluginval at strictness 5 if it's on PATH)

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
