# Third-party notices

GentSampler bundles / downloads third-party components. Their licenses and required
attributions are reproduced here. Ship these files with any GentSampler distribution.

## Basic Pitch (audio-to-MIDI transcription model)

- **What:** the `basic_pitch.onnx` weights (Spotify's ICASSP-2022 "Basic Pitch" note
  model, `nmp.onnx` from the `basic-pitch` package v0.4.0), used by the per-pad
  **TRANSCRIBE → MIDI** feature. Fetched at runtime from the model mirror; not compiled
  into the binary.
- **Copyright:** Copyright 2022 Spotify AB.
- **License:** Apache License, Version 2.0 — full text in
  [`basic-pitch/LICENSE`](basic-pitch/LICENSE).
- **NOTICE:** Spotify's attribution notice is retained verbatim in
  [`basic-pitch/NOTICE`](basic-pitch/NOTICE) (Apache-2.0 §4(d)). It lists the tools used
  to build Basic Pitch (librosa, mir_eval, numpy, pretty-midi, resampy, scipy,
  tensorflow); GentSampler redistributes only the trained ONNX model, not those libraries.
- **Source:** https://github.com/spotify/basic-pitch

No changes were made to the model file; it is redistributed as-is.

## JUCE 8.0.4 (application framework)

- **What:** the JUCE 8.0.4 framework GentSampler is built on (plugin/standalone
  scaffolding, DSP/GUI/audio-device glue). Fetched vanilla via CMake
  `FetchContent` at configure time — see `CMakeLists.txt` lines 10–14
  (`GIT_REPOSITORY https://github.com/juce-framework/JUCE.git`,
  `GIT_TAG 8.0.4`). Compiled into the binary; not a runtime download.
- **Facts (verified this pass, OQ-PKG-C):** no JUCE commercial license key
  exists anywhere in this repo; no `JUCE_DISPLAY_SPLASH_SCREEN` define exists
  anywhere in this repo; GentSampler itself has **no top-level LICENSE file**.
- **License:** JUCE 8 is dual-licensed upstream under a choice of terms — an
  AGPLv3 tier (full copyleft, free), a free tier (closed-source permitted
  below a revenue cap, conditioned on displaying the JUCE splash screen at
  startup), and a paid commercial tier (closed-source, no splash). Upstream
  terms: https://github.com/juce-framework/JUCE/blob/8.0.4/LICENSE.md and
  https://juce.com/juce-8-licence/. Notice + tier summary reproduced in
  [`juce/NOTICE`](juce/NOTICE).
- **Distribution mode:** `[UNRESOLVED — Joe to declare: AGPLv3 | JUCE free
  tier | commercial license]`
- **Flag:** if the AGPLv3 tier is elected, a top-level LICENSE file becomes
  mandatory for this repository/distribution — none currently exists. This
  entry flags that fact only; it does not create the file.
- **Source:** https://github.com/juce-framework/JUCE

## Signalsmith Stretch (time-stretch / pitch-shift engine)

- **What:** the Signalsmith Stretch DSP library used for tempo-synced
  time-stretch and master pitch-shift. Fetched via CMake `FetchContent` from
  `main` at configure time — see `CMakeLists.txt` lines 17–20
  (`GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git`,
  `GIT_TAG main`). Compiled into the binary; not a runtime download.
- **Copyright:** Copyright Signalsmith Audio Ltd.
- **License:** MIT — full text in
  [`signalsmith-stretch/LICENSE`](signalsmith-stretch/LICENSE).
- **Source:** https://github.com/Signalsmith-Audio/signalsmith-stretch

## ONNX Runtime 1.18.1 (inference engine)

- **What:** the ONNX Runtime GPU-build package used to run the stem-separation
  and transcription ONNX models. Fetched via CMake `FetchContent` at
  configure time — see `CMakeLists.txt` lines 34–38 (`ORT_VERSION "1.18.1"`,
  URL `https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/onnxruntime-win-x64-gpu-1.18.1.zip`).
  The two core CPU-path DLLs (`onnxruntime.dll`, `onnxruntime_providers_shared.dll`)
  are copied next to the deployed plugin at build time; the CUDA/cuDNN pack
  is a separate, gated first-run download (see `ModelDownloader.cpp`) and is
  not part of the default CPU-only install.
- **Copyright:** Copyright Microsoft Corporation.
- **License:** MIT — full text in
  [`onnxruntime/LICENSE`](onnxruntime/LICENSE).
- **Source:** https://github.com/microsoft/onnxruntime

## htdemucs model weights (htdemucs_ft, htdemucs_6s)

- **What:** the `htdemucs_6s` (standard) and `htdemucs_ft` (GPU max-quality)
  stem-separation model weights used by the AI stem-separation feature.
  Not compiled into the binary — downloaded at first run from the model
  mirror `https://huggingface.co/illicitish/gentsampler-models/resolve/main/`
  with SHA-256 verification (see `Source/ModelDownloader.cpp`).
- **Upstream:** Demucs (Meta Platforms, Inc. and its affiliates). Per
  `GPU_HANDOFF.md` §9 ("Locked facts / decisions"): "**MIT models only**
  (`htdemucs_ft` + `htdemucs_6s`), hosted at the HF mirror above (fp32;
  SHA-256 baked into `ModelDownloader.cpp`)."
- **License:** MIT — full text in [`demucs/LICENSE`](demucs/LICENSE).
- **Source:** https://github.com/facebookresearch/demucs

No changes were made to the model weights; they are redistributed as-is.
