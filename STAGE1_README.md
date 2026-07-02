# Stage 1 — get ONNX Runtime building into the plugin

This is **not the stem feature yet**. The goal of this stage is one thing: prove
that ONNX Runtime + DirectML downloads, compiles, and links into GentSampler on
your machine. That's the fiddly part. Once this is green, the actual stem feature
is straightforward to add (Stage 2/3).

## What changed vs your current plugin
- `CMakeLists.txt` — now auto-downloads ONNX Runtime (DirectML) and links it.
  Your `build.bat` is unchanged; it stays one-click.
- `Source/StemSeparator.h` / `Source/StemSeparator.cpp` — the stem engine (added
  to the build).
- `Source/PluginProcessor.cpp` — two small additions: an `#include`, and a few
  lines in the constructor that write a check file when the plugin loads.

Everything else is exactly your existing code.

## How to build
Same as always: run `build.bat`. The **first** configure will take longer than
usual because it downloads ONNX Runtime (~a few hundred MB) on top of JUCE.

## How to know it worked
1. After it builds and installs, open FL Studio and add GentSampler to a track
   (just loading it is enough).
2. Open this file:  **Documents\GentSampler_engine_check.txt**
3. It should say something like:
   ```
   GentSampler stem-engine check
   built: Jun 13 2026 ...
   ONNX Runtime: 1.22.1   (linked OK)
   models folder: C:\Users\JoeyD\Documents\GentSampler\models   [not found yet -- ok for Stage 1]
   ```

If you see an ONNX Runtime version line, **Stage 1 is done** — the whole engine
compiled and linked. (The "models not found" line is expected and fine; we don't
load models in Stage 1.)

Optional: if you want to also confirm the models are seen, copy your
`onnx_models` folder (the one with `manifest.json` and the 5 `.onnx` files) to
`Documents\GentSampler\models`, reload the plugin, and the log will say
`[manifest.json found]`. Still no loading/inference yet — that's Stage 2.

## If the build fails
Paste me the error. The 2 spots most likely to need a small tweak (because I
can't compile against your exact ONNX Runtime version here):

1. **"cannot open onnxruntime_cxx_api.h" / "dml_provider_factory.h not found"** —
   the NuGet's internal header path differs from what I assumed. Easy one-line
   fix to the include path once I see the error.
2. **A linker error about `Ort::` symbols** — the `.lib` path or ORT API version.
   Also a quick fix; tell me the exact message.

These are exactly why we're doing this as an isolated step — one clear thing to
get right, instead of debugging it tangled up with the UI and render code.

## What's next (once this is green)
- Stage 2: wire `StemSeparator` to your worker thread (a `doStemJob()` next to
  `doAnalysisJob()`), loading models once and running separation off the audio
  thread.
- Stage 3: a "Separate Stems" button + progress bar, store the 6 stem buffers,
  and the per-pad 6-stem mixer.
