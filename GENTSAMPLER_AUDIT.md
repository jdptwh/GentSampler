# GentSampler — Build Audit (ground truth)

**Generated:** 2026-06-28, by reading the live tree at `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` (the inner folder is the real one).
**Method:** read-only inventory across `Source/*.{h,cpp}`, `Analysis.h`, `CMakeLists.txt`. Every claim cites a real `file:line`; key/anchor citations were spot-checked against the source. Where a thing couldn't be verified it is marked **NOT FOUND** / **couldn't determine**.
**Taxonomy:** WORKING (wired end-to-end) · PARTIAL (wired but incomplete, or implemented but not connected) · STUBBED (placeholder/canned) · GATED (intentionally disabled behind a flag) · DEAD (orphaned/no caller).

> **Read the "Biggest surprises" section at the end first** if you only read one part — it lists what contradicts the design docs / session history (notably: there is **no Python sidecar**; the inspector has **no disclosure**).

---

## A. Audio engine & per-pad playback

### Sample load path
State: WORKING
Evidence: PluginProcessor.h:124, PluginProcessor.cpp:170-213
Notes: `loadFile()` reads an AudioBuffer into SourceSample (max ~10 min). Audio thread reads an `active` copy under SpinLock. Grid slicing applied at load (default 16 equal pads).

### Per-pad voice playback & polyphony
State: WORKING
Evidence: PluginProcessor.h:278-299 (Voice struct), PluginProcessor.cpp:1400-1546 (startVoice/releaseVoices), processBlock voice loop ~2108-2326, `std::array<Voice,32> voices` at PluginProcessor.h:399
Notes: 32-voice pool. `startVoice()` takes a free slot, else steals `voices[0]`. Voices are dual-source: `srcKind=0` (master render) or `srcKind=1` (per-pad speed render). Generation matching detects stale renders.

### Per-pad filter (TPT state-variable)
State: WORKING
Evidence: PluginProcessor.h:57-59 (cutoff/reso/ftype params), PluginProcessor.cpp:2218-2307
Notes: Zavalishin TPT multimode SVF per-sample. Off/LP/HP/BP; cutoff 20Hz–20kHz, reso→Q 0.5–8; state cleared on voice start; coeffs per block (live sweeps).

### Attack/release envelope
State: WORKING
Evidence: PluginProcessor.h:287, PluginProcessor.cpp:1452-1453, 2309-2318
Notes: Linear attack/release, state machine 0 attack/1 sustain/2 release; voice ends when env ≤ 0.

### Choke groups & voice stealing
State: WORKING
Evidence: PluginProcessor.cpp:1424-1449
Notes: Non-zero choke group cuts other pads in that group with a ~4 ms fade; per-pad mode (gate/one-shot/latch); LATCH toggles off if already sounding.

### Per-pad pitch-shift (repitch)
State: WORKING
Evidence: PluginProcessor.h:47, PluginProcessor.cpp:1454, 1467-1468, render loop ~2252-2273
Notes: Classic repitch via playback rate `(srcSR/fs) * 2^(semis/12)` with linear interpolation. Not formant-preserving (it's a resample, by design).

### Per-pad time-stretch (SPEED) — Signalsmith
State: WORKING
Evidence: PluginProcessor.h:53, PluginProcessor.cpp:755-862 (doPadRenderJobs), 835 (offlineStretchSlice)
Notes: **SignalsmithStretch** (phase-vocoder). Pads with speed ≠ 1.00× pre-stretch their slice offline into `PadRender::buffer`, cached by generation. Default preset (no explicit quality dial). This is the same engine that produces the tempo-stretch artifacts noted in the stem-quality work.

### Per-pad crush (SP-1200 style)
State: WORKING
Evidence: PluginProcessor.h:292-294, PluginProcessor.cpp:2277-2289
Notes: Sample-rate hold + bit quantize; crush 0–1 → hold count and bit depth.

### Aux-bus routing (per-pad stereo out)
State: WORKING
Evidence: PluginProcessor.cpp:14-18 (makeBuses), 2137-2155
Notes: 16 optional per-pad stereo buses; a voice routes to its pad's bus if enabled, else main.

### Master time-stretch & host tempo sync
State: WORKING
Evidence: PluginProcessor.cpp:1303-1394 (doRenderJob), 2011-2015
Notes: Whole sample stretched once (Signalsmith) to `currentTargetSpeed()`; all 16 pads read the master render → cheap tempo follow.

### Granular engine attach-point
State: NOT FOUND (no granular code exists)
Evidence: grep for granular = 0 hits; voice loop PluginProcessor.cpp:~2235-2324, `srcKind` at PluginProcessor.h:290
Notes: A per-pad granular path would attach in the per-sample voice loop as a **third `srcKind`** with a `GranularRender` sibling to `PadRender`. The loop is buffer-source-agnostic (resolves `vinL/vinR/vlen` by srcKind, then reads with linear interp), so granular can coexist with the current playback path. **Rewrite risk LOW** as an added branch; HIGH only if it replaces the core interp read. (See priority P2.)

---

## B. Slicing & window system

### (1A) Selected-pad handles win at shared boundaries
State: WORKING
Evidence: PluginEditor.h:470-490
Notes: mouseDown tests the selected pad's own start/end handles first; closer-handle wins; right-click on its end resets to auto.

### (1B) Collapsed window → OPEN / gated slice
State: WORKING
Evidence: PluginProcessor.h:132-137 (`kOpenSlice = -2`), PluginProcessor.cpp:354-364 (setCueEnd collapse), 1475-1505 (playback ignores boundary, runs to sample end), PluginEditor.h:300-313 (▶ gate affordance), ~567 (8px pixel-based collapse/re-open)
Notes: Collapsed window (cueEnd ≤ cue+32) becomes the `kOpenSlice` sentinel: plays from the start, ignores stop-at-cue/next-cue, gate-release / one-shot-to-end decides the cut. Round-trips through persistence. Re-open is pixel-based.

### (1C) Active-window highlight
State: WORKING
Evidence: PluginEditor.h:105-118 (selected region fill α 0.22 vs 0.09), 258-260 (2px selected start line), 274 (brighter flag), flag-click select
Notes: Selected pad's window/flag/edges emphasized; clicking a flag selects that pad.

### (2A) Music-aware auto-slice (transients + grid + reconcile)
State: WORKING
Evidence: PluginProcessor.cpp:571-592 (autoSliceMusical), 506-569 (computeBlendedSlices), Analysis.h onsets
Notes: Onsets (position+strength) reconciled to the beat grid — sensitivity gates onset strength (High 0.06 / Med 0.20 / Low 0.45), snap tolerance scales with the snap setting (Tight 0.5 / Med 0.3 / Loose 0.12 of a grid step), clusters merged (keep strongest), top-16, pad 1 forced near 0. No reliable BPM → pure transient slicing.

### (2B) Visible beat grid + snap-to-grid
State: WORKING
Evidence: PluginEditor.h:212-241 (grid draw when SNAP on), PluginProcessor.cpp:458-469 (gridStepSamples), 481-504 (snap), nearestGridLine
Notes: Faint bar/beat/subdivision grid drawn only when SNAP is on (auto-thinned when zoomed out). Snap pulls cue placement and the window end to the grid; falls back to nearest transient when there's no tempo. Live scrub cursor also snaps to grid + placed cues.

### Slice analysis location
State: WORKING
Evidence: Analysis.h (`Analyzer::analyze`), PluginProcessor.cpp:864-897 (doAnalysisJob, worker thread)
Notes: Offline on the worker thread; spectral flux (FFT 1024/hop 512), peak-pick top-16 + full onset list, atomic-swapped under infoLock.

### Per-slice classification metadata (hook-point)
State: NOT FOUND (no per-slice type/category stored)
Evidence: Analysis.h:22-23 (AnalysisResult = slices[int] + onsets[pair<int,float>] only)
Notes: Only onset **strength** is retained per slice. A classifier would add a metadata channel to AnalysisResult and consume it in computeBlendedSlices. (See priority P5.)

---

## C. Stem separation

### Engine architecture — IN-PROCESS ONNX (no Python sidecar)
State: WORKING
Evidence: StemSeparator.h:1-90, StemSeparator.cpp:12-20 (ORT_API_MANUAL_INIT), CMakeLists.txt:85-88
Notes: **Pure in-process C++ via ONNX Runtime 1.18.1, manual DLL load (`onnxruntime.dll`) — there is NO Python sidecar at runtime.** Headers linked at compile time; DLL loaded lazily on the worker thread so plugin load has zero ONNX dependency. (Python only ever existed as the offline export/PoC, since deleted.)

### Hybrid model path (ft + 6s) — real
State: WORKING
Evidence: StemSeparator.cpp:749-754, 826 (other = ft.other − 6s.guitar − 6s.piano), PluginProcessor.cpp:1020-1022 (mode select)
Notes: `Mode::Hybrid` runs htdemucs_ft (drums/bass/vocals + ft.other) **plus** htdemucs_6s (guitar/piano), then computes residual `other`. Selected by the quality dial (q≥2 → Hybrid, else SixStem). NOT 6s-alone. **Caveat:** guitar/piano use 6s in *both* standard and hybrid, so "MAX" does not change the guitar stem — only drums/bass/vocals improve.

### Model download & cache
State: WORKING
Evidence: ModelDownloader.cpp:86-207, 218-276
Notes: HF mirror `illicitish/gentsampler-models` (~1.79 GB), SHA-256 verify, resumable `.part`, atomic rename, manifest.json written to `~/Documents/GentSampler/models`.

### GPU / CUDA
State: GATED
Evidence: StemSeparator.cpp:50 (`kEnableCuda = false`), 289 (gate on probe), 660 (`gpuProviderResolved = kEnableCuda && …`)
Notes: Cleanly gated — the CUDA provider is only attempted when `kEnableCuda` is true; CPU path always reachable. No partial reachability. (GPU proven in standalone Python but crashes inside FL — see GPU_HANDOFF.md.)

### Stem retention (residual stems)
State: WORKING — all 6 retained
Evidence: PluginProcessor.h:40-52 (StemSet `std::array<AudioBuffer,6>`), PluginProcessor.cpp:1057-1070 (all 6 stored), StemSeparator.cpp:810-847 (all 6 assembled incl. computed `other`)
Notes: **All six stems are kept unconditionally** in both source (`StemSet.buffers[6]`) and rendered (`RenderedStems.buffers[6]`) domains, and dumped as proof WAVs to `stems_out/`. Selection is by gain mask at playback, not by discarding. (See priority P1 — bleed control is an afternoon, not a re-arch.)

---

## D. Export / drag-out

### Pad WAV drag-out
State: WORKING
Evidence: PluginEditor.h:1024-1063 (DragChip), PluginEditor.cpp:236-241, PluginProcessor.cpp:1799-1820 (exportPad → 24-bit stereo WAV)
Notes: Real rendered file (cue→end, per-pad pitch/level/crush/speed baked) written to temp, exposed as an OS drag source. NOTE: the drag *chip* is currently hidden (see F); pad-WAV export is reachable via the EXPORT menu save-as.

### MIDI capture & drag-out
State: WORKING
Evidence: PluginProcessor.cpp:1596-1619 (capture), 1627-1686 (exportCapturedMidi → SMF), PluginEditor.cpp:242-245 (midiChip), 441-447 (REC MIDI)
Notes: Records pad hits during a session → Standard MIDI File (960 TPQ, tempo meta, pads as C#1..B2), draggable. This is **performance-capture** MIDI, not audio-to-MIDI.

### Kit export (folder)
State: WORKING
Evidence: PluginProcessor.cpp:1867-1907, PluginEditor EXPORT menu
Notes: 16 pad WAVs + a preview .mid + a FlipLog.txt (BPM/key/cues/per-pad settings). Folder, not an archive.

### Per-pad export (save dialog)
State: WORKING
Evidence: PluginEditor.cpp EXPORT menu "Export selected Pad as WAV…", PluginProcessor.cpp:1799-1820

### Rendered-slice assembly (hook-point)
State: WORKING
Evidence: PluginProcessor.cpp:1692-1797 (renderPadSlice)
Notes: Single source of truth for all exported slice audio (pitch/level/crush/speed). One place to finish/polish drag-out and to attach a future audio-to-MIDI extractor.

### Full-sequence / master render export
State: NOT FOUND
Evidence: no exportSequence/exportMaster in PluginProcessor.{h,cpp}
Notes: Only per-pad + kit-folder. No full performance/master WAV.

### Audio-to-MIDI extraction
State: STUBBED / absent
Evidence: only performance-capture MIDI exists (1627-1686)
Notes: True audio→MIDI (onset/pitch segmentation of a slice) is not present; would attach at renderPadSlice + Analysis.h onsets.

---

## E. UI / UX (post-redesign)

### Top toolbar
State: WORKING
Evidence: PluginEditor.cpp:743-756
Notes: LOAD · KIT▾ · EXPORT▾ on the left; KEYBOARD / REC MIDI / MIDI demoted right. **SLICE was moved out of the toolbar to the waveform tools row** (PluginEditor.cpp:789).

### Full-width waveform + bottom split (pads-left / inspector-right)
State: WORKING
Evidence: PluginEditor.cpp:764-812
Notes: Full-width sample map (stem lanes, cue flags, playheads) with tools row PREVIEW · SNAP · FOLLOW · SLICE … FULL VIEW; pads ~56% left, inspector right.

### Inspector — ALL controls exposed, NO disclosure
State: WORKING (but diverges from the redesign brief)
Evidence: PluginEditor.cpp:814-910, 558-559 (padCueLbl + routeLbl removed)
Notes: **The brief expected CHOKE/FILTER/ROUTING under a "▸ ADVANCED" disclosure. There is no disclosure** — every control is exposed in a fixed layout: SELECTED PAD (number+meta) · TRIGGER icons · LOOP/REVERSE · PAD SOURCE (FULL+6) · PLAYBACK (pitch/speed/level/pan) · SHAPE (att/rel/crush) | FILTER (cutoff/reso/type) · CHOKE + STOP-AT-CUE. The **ROUTING readout was removed entirely**. (This was a deliberate change made with Joe after the redesign doc was written.)

### Controls backed by logic
State: WORKING
Evidence: PluginEditor.cpp:964-978 (attachPad rebinds APVTS), TrigPad → playMode (line 85), PAD SOURCE → stem masks (line 408)
Notes: All exposed knobs/dropdowns/toggles are bound to processor params. The only UI element not backed by a live feature is the hidden GPU toggle (see F).

### Stem separation QUALITY dial
State: WORKING
Evidence: PluginEditor.cpp:777 (qualityBox, replaced the hidden gpuBtn)
Notes: FAST / HQ / MAX in the map header → `stemQuality` (overlap + hybrid model selection).

---

## F. Consolidated DEAD / GATED / STUBBED inventory

| Item | State | Evidence | Note |
|---|---|---|---|
| `kEnableCuda` GPU chain | GATED | StemSeparator.cpp:50, 289, 660 | Hard-off; CPU always used. Cleanly gated. |
| `gpuBtn` GPU toggle | DEAD | PluginEditor.h:1276, .cpp:427-429, 778 | Constructed, `setVisible(false)`, replaced by qualityBox. |
| `stemUseGpu` | PARTIAL | PluginProcessor.h:193, .cpp:1002 | Read at init, only set by the hidden gpuBtn → effectively always false. |
| `canUseMaxQuality` / `stemCanUseMaxQuality` | STUBBED | StemSeparator.cpp:678-682 | Never called; always false (GPU gated). |
| `stemMaxQuality` sentinel | PARTIAL | PluginProcessor.cpp:689, 1021 | Legacy `separate_maxq.txt` test hook; not in normal use. |
| `Mode::FourStem` | PARTIAL | StemSeparator.cpp:838-841 | Assembly exists but never selected (only Hybrid/SixStem used). |
| `stemBtn[6]` lane mute buttons | DEAD | PluginEditor.cpp:373-383, 795, 1174-1178 | Hidden + zero-bounds; still updated by the timer each frame (wasted). Mute/solo now via the WaveformView lane pills. |
| `padWavChip` drag chip | DEAD | PluginEditor.cpp:236-247, 556 | Constructed then hidden; pad-WAV export moved to the EXPORT menu. |
| `padCueLbl` (cue-range) | DEAD | PluginEditor.cpp:64, 558, 1089-1098 | Removed from inspector; still text-updated by the timer. |
| `routeLbl` (routing) | DEAD | PluginEditor.cpp:70-72, 559, 1104-1105 | Removed from inspector; still text-updated by the timer. |
| `playMode` combo | PARTIAL (intentional) | PluginEditor.cpp:39-45, 85, 836-837 | Hidden; driven by the TrigPad icon buttons + APVTS attachment. Working by design. |
| Sentinel files `separate_now/gpu/maxq.txt` | WORKING | PluginProcessor.cpp:674, 681-691 | Headless test hooks; checked+deleted each loop. |

Cleanup opportunity: several hidden labels/buttons (`stemBtn`, `padCueLbl`, `routeLbl`) are still written by `timerCallback` every frame — harmless but dead work.

---

## G. Build & infrastructure

### JUCE / build config
State: WORKING
Evidence: CMakeLists.txt:10-14, 59-69
Notes: JUCE 8.0.4 (FetchContent), VST3, C++17, VS2022. Release: `cmake --build build --config Release --parallel`. Output `build/GentSampler_artefacts/Release/VST3/GentSampler.vst3`. (`build.bat` exists but is admin-xcopy/`pause` — avoid in automation.)

### ONNX Runtime integration
State: WORKING
Evidence: CMakeLists.txt:30-51 (ORT 1.18.1 GPU zip), StemSeparator.cpp:60-150 (manual `LoadLibraryExW` + `GetProcAddress`, ORT_API_MANUAL_INIT)
Notes: Manual runtime load (no link-time dep) for FL's restricted DLL search. Model segment 343,980 samples (7.8 s @ 44.1 k). Provider chain CUDA→CPU (CUDA gated).

### DLL bundling & first-run download
State: WORKING
Evidence: CMakeLists.txt:104-114 (POST_BUILD copies onnxruntime.dll + providers_shared), ModelDownloader.{h,cpp}
Notes: Small core DLLs bundled next to the binary; large CUDA EP + cuDNN runtime + the model weights are first-run downloads to the models folder. This download-and-verify pattern is a clean template for a **second sidecar** later.

### Signalsmith Stretch + vendored libs
State: WORKING
Evidence: CMakeLists.txt:16-20, 80-83
Notes: MIT header-only stretch/pitch engine via FetchContent.

### Analysis.h FFT infrastructure
State: WORKING (analysis) / PARTIAL (reuse exposure)
Evidence: Analysis.h:50-52 (FFT 1024/hop 512), 86-160 (flux + autocorr BPM 60–180 + top-16 transients + full onset list), 172-204 (chromagram FFT 4096 + Krumhansl key)
Notes: Solid reusable DSP for onsets/BPM/key. **But** flux curve, per-frame magnitudes, and chroma are computed and discarded — only onset position+strength survive. A granular or audio-to-MIDI front-end could reuse the FFT path but would currently re-analyze (nothing cached beyond onsets).

---

## Prioritized answers

**P1 — Does the stem engine keep the residual (non-selected) stems? → YES (high confidence).**
All 6 stems are retained in `StemSet.buffers[6]` (PluginProcessor.h:40-52) and `RenderedStems.buffers[6]`; all 6 are mixed per-voice with gain masks (PluginProcessor.cpp:2184-2209, 2254-2268) and stretched per-pad (845-851). Nothing is discarded. **Bleed control is an afternoon (mask/gain work), not a re-architecture.**

**P2 — Can the per-pad voice host a second (granular) render mode without a rewrite? → YES (high confidence).**
`Voice::srcKind` (PluginProcessor.h:290) already abstracts master-render vs per-pad-render; the per-sample loop (PluginProcessor.cpp:2235-2324) reads a buffer pointer with linear interp and applies crush/filter/env/pan mode-agnostically. Add `srcKind=2` + a `GranularRender` job (sibling to `doPadRenderJobs` at 755) + a grain branch around the sample read. Downstream DSP needs no change.

**P3 — Real state of drag-out? → WORKING; polish, not a gap (high confidence).**
Pad WAV (24-bit stereo, real rendered file via `renderPadSlice`→`exportPad` 1692/1799), kit-folder export, and performance-capture MIDI (`exportCapturedMidi` 1627) are all real and wired. Gaps are scope, not bugs: no full-master/sequence export, no audio-to-MIDI, and the pad-WAV *drag chip* is currently hidden (export is via the EXPORT menu).

**P4 — Slice/window overhaul completeness vs SLICE_WINDOW_TASK.md? → YES, all items wired (high confidence).**
1A (PluginEditor.h:470-490), 1B (`kOpenSlice` PluginProcessor.h:134 + playback 1475-1505 + UI 300-313), 1C (PluginEditor.h:105-118, 274), 2A (computeBlendedSlices 506-569 + menu controls), 2B (grid draw 212-241 + snap 481-504). No stubs found.

**P5 — Per-slice spectral/feature data a classifier could reuse? → PARTIAL (high confidence).**
Only per-onset **strength** (0..1) is stored (`transientOnsets`, Analysis.h:23, 158-159; used in computeBlendedSlices). The richer features (flux curve, per-frame magnitude spectra, chromagram) are computed in `Analyzer::analyze` and **discarded** (Analysis.h:49-81, 172-200). A classifier would either cache those during analysis (cheap change to AnalysisResult) or re-analyze.

---

## Biggest surprises (contradicts docs / session history)

1. **No Python/Demucs sidecar exists.** The brief (area C) assumes a "Python sidecar boundary." Reality: stem separation is 100% **in-process C++ via ONNX Runtime** with manual DLL loading (StemSeparator.cpp). Any plan that assumed launching/feeding a Python process is moot — and adding a *second* model is "extend the in-process ONNX engine + the first-run download," not "add a sidecar."

2. **The inspector has no disclosure, and ROUTING is gone.** The redesign brief (area E) expected CHOKE/FILTER/ROUTING "under a ▸ ADVANCED disclosure." The shipped UI **exposes everything** in a fixed layout and **removed the ROUTING readout entirely** (PluginEditor.cpp:558-559). This was an intentional post-redesign change; plan UI work against the *exposed* layout, not the disclosure.

3. **"MAX" quality does not improve guitar/piano.** Hybrid uses htdemucs_6s for guitar/piano in *both* standard and MAX (StemSeparator.cpp:749-754). The ft upgrade only touches drums/bass/vocals. Guitar artifacts are dominated by the Signalsmith tempo-stretch, not the model.

4. **A meaningful amount of constructed-but-dead UI** (gpuBtn, stemBtn[6], padWavChip, padCueLbl, routeLbl) — several still written by the 15 Hz timer every frame. Plus unreachable engine paths: `Mode::FourStem`, `canUseMaxQuality`, `stemUseGpu`. None harmful, but it's quietly half-wired.

5. **Rich analysis is computed then thrown away.** Spectral flux, per-frame magnitudes, and chroma all exist in `Analyzer::analyze` but only onset position+strength survive — so the slice-classifier and any audio-to-MIDI feature start from "cache what's already computed," a small win that isn't taken yet.

6. **Drag-out is more complete than the brief implies, and `renderPadSlice` is a clean single hook** (PluginProcessor.cpp:1692) for finishing drag-out polish and attaching audio-to-MIDI later — but the pad-WAV drag *chip* itself is hidden, so the discoverable path today is the EXPORT menu.
