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
