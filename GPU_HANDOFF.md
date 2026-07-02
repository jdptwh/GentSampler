# GPU_HANDOFF.md — GentSampler CUDA/GPU integration

**Purpose:** self-contained handoff so a fresh session (zero prior context) can continue the GPU work.
**Last updated:** 2026-06-25. **Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` (inner folder is the real one).

---

## 0. READ THIS FIRST — the state has moved past the "1.22 / Add-node" framing

If you were told the current blocker is `CUDA inference failed (code 1): Add node '/Add_1' — cudaErrorInvalidResourceHandle` on **ORT 1.22**, that is **stale**. That error belonged to the ORT 1.22 / cuDNN 9 path, which we **abandoned**. Since then:

- We **downgraded to ORT 1.18.1 + CUDA 11.8 + cuDNN 8.9** (the legacy cuDNN-8 conv path).
- On that stack the model runs the **full 7.8 s segment CLEAN on the RTX 2060 in a standalone Python process** (proven — see `StemTest/test_legacy_cuda.py`). No conv-frontend wall, no Add error.
- **But inside FL Studio's process it CRASHES** the host. That is the **current blocker** (Section 3).
- **The thread/context + `cudaSetDevice(0)` fix is ALREADY IMPLEMENTED and did NOT fix it.** In our architecture the ORT session is **created and Run() on the same worker thread** — there is *no* thread mismatch to fix (Section 5, "Threading model"). Do **not** make "put Run on the same thread + cudaSetDevice" step 1; it's done.
- CUDA is currently **gated OFF** (`static const bool kEnableCuda = false;` in `StemSeparator.cpp`) so the GPU opt-in **cannot crash FL**. **CPU is the shipping path.**

---

## 1. One-line status

GPU/CUDA is **groundwork-complete and PROVEN to run on the RTX 2060 standalone**, but is **gated off** because the CUDA/cuDNN libraries crash when loaded into **FL Studio's process** (a host-process integration fault, not a model/hardware fault). **CPU separation works fully** (~33–38 s, full quality, cross-platform). **GPU inside FL does not work yet.**

---

## 2. The journey and the dead-ends (do not repeat these)

### Phase A — ORT 1.22.0 (Gpu.Windows) + CUDA 12.6 + cuDNN 9.23.2 — ABANDONED
1. **cuDNN-9 conv frontend can't build htdemucs's first conv** (input `[1,2,343980]`, a 343,980-wide 1-D conv): `Failed to initialize CUDNN Frontend … CUDNN_BACKEND_API_FAILED` at `conv.cc build_operation_graph`.
   - Tried `cudnn_conv1d_pad_to_nc1d=1` → reshaped conv to `[1,2,1,343980]`, **still failed**.
   - Tried `cudnn_conv_algo_search=HEURISTIC` + `use_tf32=0` → **no change**.
   - Research verdict: ORT 1.20+ made the cuDNN-9 frontend mandatory with **no legacy fallback**; this is a known regression. No provider-option fix.
2. **Smaller export segment shrinks the conv** (the real lever): the export must set `model.segment` (htdemucs `use_train_segment=True` otherwise pads any input back up to the 7.8 s training length, keeping the conv at 343,980). With `export_onnx.py --seconds 2.0` **and** `model.segment` set, the conv became `[1,2,1,88200]` and **the conv PASSED**.
3. **Then hit `cudaErrorInvalidResourceHandle` at `/Add_1`** (an elementwise Add — no cuDNN/cuBLAS). Research: a 1.21/1.22 CUDA **stream/allocator lifecycle regression**.
   - Tried `do_copy_in_default_stream=1` + `arena_extend_strategy=kSameAsRequested` + `enable_cuda_graph=0` → **no change**.
   - Tried `cudaSetDevice(0)+cudaFree(0)` on the worker thread → **CRASHED FL** (on the 1.22 stack).
   - Tried `CUDA_MODULE_LOADING=EAGER` → **regressed the conv** (eager-loads cuDNN engine modules → VRAM pressure on the 6 GB card → conv `build_operation_graph` fails again). Reverted.
   - Also observed **non-determinism**: the same 88,200 conv passed on some runs, failed on others.
   - Conclusion: 1.22 + cuDNN 9 is a dead end for this model/GPU.

### Phase B — ORT 1.18.1 + CUDA 11.8 + cuDNN 8.9.5 — CURRENT (legacy path)
4. **De-risked in Python (CLEAN):** `StemTest/test_legacy_cuda.py` runs the **full 7.8 s `htdemucs_6s`** (conv 343,980 + Add) on `CUDAExecutionProvider` with ORT 1.18.1 + cuDNN 8.9.5 on the RTX 2060 — **SUCCESS**, output produced, no conv-frontend wall, no Add error. (Legacy cuDNN-8 conv path + older stable ORT stream architecture.) **This proves the model + GPU are fine.**
5. **Committed to the plugin:** CMake → ORT 1.18.1 GitHub zip; bundle → ORT 1.18 + CUDA-11.8/cuDNN-8.9 DLLs; preload list → cuDNN-8 names; CUDA provider options → **defaults** (no workarounds needed on legacy path); `cudaSetDevice(0)` (from `cudart64_110.dll`) on the worker thread before session creation.
6. **Result in FL: CRASHES on GPU separate.** Plugin **loads** fine (engine-check: `ONNX Runtime 1.18.1 loaded OK, CUDA EP available, 11/11 DLLs`), CPU works; only the **GPU inference** crashes.
7. **Tried `cudaSetDevice(0)` (the documented worker-thread fix) → still crashed.**
8. **Stabilized:** `kEnableCuda=false` gates CUDA off → GPU opt-in can't crash FL. CPU ships.

---

## 3. THE CURRENT BLOCKER (exact)

**FL Studio (FL64.exe, v21.0.3) crashes the moment a GPU separation is triggered.** Plugin load is fine; the crash is during CUDA **inference on the worker thread**. From the Windows **Event Log → Application → "Application Error"** (two faulting modules across repeated crashes):

```
Faulting module: cudnn64_8.dll  v8.9.5.29   Exception 0xC0000409  fault offset 0x000000000001df51
    (0xC0000409 = STATUS_STACK_BUFFER_OVERRUN — a __fastfail; signature of Control-Flow-Guard / stack-cookie)
Faulting module: onnxruntime_providers_cuda.dll  (1.18)  Exception 0xC0000005  fault offset 0x00000000005e9751
    (0xC0000005 = access violation)
Faulting application: C:\Program Files\Image-Line\FL Studio 21\FL64.exe
```

- **When:** first/only GPU separation in the session (the plugin chunks the audio and runs the model per chunk; it crashes during inference). The **identical model + input runs clean in standalone Python**, so it is **not** the model, the conv, or the hardware.
- **Interpretation:** host-process integration fault — FL's process mitigations (Control Flow Guard / `__fastfail` security checks) and/or a CUDA-context/ABI collision inside FL's process. The libs themselves work in a clean (Python) process.
- **NOTE:** the older `cudaErrorInvalidResourceHandle @ /Add_1` is the **abandoned-1.22** blocker. Don't chase it.

---

## 4. Verified environment

### Current (what's in the code/bundle NOW) — ORT 1.18.1 / CUDA 11.8 / cuDNN 8.9
- **ONNX Runtime 1.18.1**, GPU build, from GitHub `onnxruntime-win-x64-gpu-1.18.1.zip` (CUDA 11.8 + cuDNN 8.x build; this is the legacy cuDNN-8 conv path).
- **CUDA 11.8 runtime** (from `nvidia-*-cu11` pip wheels): `cudart64_110.dll`, `cublas64_11.dll`, `cublasLt64_11.dll`, `cufft64_10.dll`, `curand64_10.dll`, `nvrtc64_112_0.dll`, `nvrtc-builtins64_118.dll`.
- **cuDNN 8.9.5.29** (`nvidia-cudnn-cu11==8.9.5.29`): `cudnn64_8.dll` + `cudnn_ops_infer64_8.dll`, `cudnn_cnn_infer64_8.dll`, `cudnn_adv_infer64_8.dll`.
- **14 DLLs total** in the bundle next to `onnxruntime.dll` (= the 11 above + `onnxruntime.dll` + `onnxruntime_providers_shared.dll` + `onnxruntime_providers_cuda.dll`), at `…\GentSampler.vst3\Contents\x86_64-win\`.

### Abandoned (the original framing) — ORT 1.22.0 / CUDA 12.6 / cuDNN 9.23.2
- ORT **1.22.0** (`Microsoft.ML.OnnxRuntime.Gpu.Windows`). CUDA **12.6**: `cudart64_12` (12.6.0), `cublas64_12`/`cublasLt64_12` (12.6.4), `cufft64_11` (11.3.0). cuDNN **9.23.2.1**: `cudnn64_9` + sublibs (`cudnn_graph64_9`, `cudnn_ops64_9`, `cudnn_engines_precompiled64_9`, `cudnn_engines_runtime_compiled64_9`, `cudnn_heuristic64_9`, `cudnn_adv64_9`, `cudnn_cnn64_9`). **13 CUDA DLLs.** No longer used — kept here only so the dead-ends make sense.

### Hardware / driver
- **NVIDIA RTX 2060**, Turing **SM75**, **6 GB**, driver **566.03** (supports CUDA 11.8 and 12.x).

### How ORT is loaded (manual — do NOT "fix" to normal linking; FL's restricted DLL search needs this)
In `StemSeparator.cpp :: ensureOrtLoaded()` (runs once, lazily, on the worker thread):
1. Resolve the plugin's own folder: `GetModuleHandleExW(FROM_ADDRESS, &ensureOrtLoaded)` → `GetModuleFileNameW`.
2. `LoadLibraryExW("onnxruntime.dll", LOAD_WITH_ALTERED_SEARCH_PATH)` from that folder (so its deps resolve there).
3. **Pre-load every CUDA/cuDNN DLL by name** from that folder with `LOAD_WITH_ALTERED_SEARCH_PATH` (the `kCudaDlls[]` list). **This is required** — `onnxruntime_providers_cuda.dll`'s transitive imports (cudnn/cublas/cudart) are otherwise not found (LoadLibrary **error 126**). Was a real, necessary fix.
4. Resolve `OrtGetApiBase` via `GetProcAddress`, `GetApi(ORT_API_VERSION)`, `Ort::InitApi(api)` (the **ORT_API_MANUAL_INIT** pattern — manual, no auto-init).
5. Resolve EP append fns via `GetProcAddress`: `g_cudaAppend = OrtSessionOptionsAppendExecutionProvider_CUDA`, `g_dmlAppend`. Null = provider not in this binary → chain skips it.
6. Resolve `g_cudaSetDevice` from `cudart64_110.dll` (for the worker-thread bind).

`onnxruntime.dll` + `onnxruntime_providers_shared.dll` are bundled (POST_BUILD); `onnxruntime_providers_cuda.dll` + the CUDA/cuDNN runtime are large → intended to be a **first-run download pack**, currently hand-placed.

---

## 5. What's implemented in code (files + what each change does)

### `Source/StemSeparator.cpp` / `.h` (the engine)
- **Provider-priority chain** in `makeSession()`: an **ordered list of attempts** `[CUDA, CPU]`; each builds a **fresh `Ort::SessionOptions`**, runs its `configure` lambda (may throw if the EP can't attach), tries to construct the `Ort::Session`; **first success wins** and sets `selectedProvider`/`gpuActive`; failures log + **fall through** to the next. CPU is the always-succeeds terminal. **Clean future-insert points** (commented) for CoreML (macOS), WebGPU, DirectML — each provider's quirks stay *inside its own lambda*.
- **CUDA append** uses the **V2 options API**: `Ort::GetApi().CreateCUDAProviderOptions` → `SessionOptionsAppendExecutionProvider_CUDA_V2` with **default options** (no workarounds; legacy path needs none).
- **`kEnableCuda` flag** (file-scope `static const bool`, **currently `false`**): gates the CUDA chain entry, the `cudaSetDevice` hook, and `gpuProviderResolved`. **Flip to `true` to resume GPU.**
- **`cudaSetDevice(0)`** (resolved from `cudart64_110.dll`) called on the worker thread **before** session creation (gated by `kEnableCuda`).
- **DLL pre-load** list `kCudaDlls[]` (cuDNN-8 / CUDA-11 names) + `g_cudaDllsFound` count (shown in engine-check).
- **Manifest-driven segment:** `Impl::segmentSamples` read from manifest (`segment_samples`, else `segment*samplerate`, else default 343980) — lets you swap models of different segment length **without recompiling**.
- **Quality gating:** `canUseMaxQuality()` = `useGpu && gpuProviderResolved && (htdemucs_ft present)`; `separate()` downgrades `Hybrid/FourStem` → `SixStem` when no GPU. `selectedProvider()` returns `"CUDA"|"CPU"`.
- **Diagnostics:** `usingGPU()`, `deviceFellBackToCpu()`, `gpuErrorMessage()`; `gentCheckStemEngine()` writes the engine-check file.
- **`runModelFile()`** has a 2-attempt loop: GPU then **forced CPU** on any `Ort::Exception` (graceful fallback). **Caveat:** a *hard crash* (the current blocker) is **not** a catchable `Ort::Exception`, so this fallback can't save it — that's why CUDA had to be gated off rather than relying on try/catch.

### `Source/PluginProcessor.cpp` / `.h`
- `doStemJob()` (worker thread): chooses `mode = (stemMaxQuality && canUseMaxQuality()) ? Hybrid : SixStem`; **`htdemucs_6s` is the standard default**, **`htdemucs_ft` is GPU-only max quality**. Status/log use `selectedProvider()`; `gpuErrorMessage()` appended to `stems_log.txt`.
- Sentinel test hooks in `run()`: `separate_now.txt` (CPU), `separate_gpu.txt` (CUDA standard), `separate_maxq.txt` (CUDA + ft).
- `stemUseGpu`, `stemMaxQuality` atomics + accessors; `stemCanUseMaxQuality()`, `stemSelectedProvider()`.

### `CMakeLists.txt`
- ORT package: **GitHub `onnxruntime-win-x64-gpu-1.18.1.zip`** (was `Gpu.Windows` 1.22.0). `file(GLOB_RECURSE)` finds `onnxruntime_cxx_api.h` → derives `ORT_INCLUDE` (include/) and `ORT_NATIVE_DIR` (lib/).
- We do **not** link onnxruntime (manual runtime load). `juce::juce_cryptography` linked (for `juce::SHA256` in the downloader).
- POST_BUILD copies only `onnxruntime.dll` + `onnxruntime_providers_shared.dll`; the CUDA/cuDNN DLLs + `providers_cuda` are bundle-assembled manually (future: download pack).

### `Source/ModelDownloader.cpp` / `.h`
- First-run weight downloader: HF mirror `https://huggingface.co/illicitish/gentsampler-models/resolve/main/`, **SHA-256 verify**, resumable (`.part` + Range), atomic rename, writes `manifest.json`. For the **5 fp32 models**. **Works** (this is the CPU shipping path's model acquisition). Windows uses native WinINet (no libcurl / `JUCE_USE_CURL` needed).

### `StemTest/export_onnx.py`
- `--fp16` flag: post-hoc `onnxconverter_common.float16.convert_float_to_float16(keep_io_types=True)` → `*.fp16.onnx` (fp32 IO, fp16 weights). Emits `onnx_files_fp16` in manifest.
- Sets `model.segment = seconds` when `--seconds` given (so the conv actually shrinks); writes `segment` + `segment_samples` to manifest.

### `StemTest/test_legacy_cuda.py`
- The standalone CUDA de-risk (KEEP). Creates a venv with `onnxruntime-gpu==1.18.1` + `nvidia-cudnn-cu11==8.9.5.29` + `nvidia-{cublas,cufft,curand,cuda-runtime,cuda-nvrtc}-cu11`, **pre-loads the DLLs by name** (ORT 1.18 doesn't auto-discover them and `os.add_dll_directory` doesn't cover transitive imports), runs the full 7.8 s model on `CUDAExecutionProvider`. **Reproduces the PROOF that the model runs on the GPU.**

### THREADING MODEL — CRITICAL, read carefully
- The `Ort::Env` is created **once** in `initialise()` and **reused** across re-inits (CPU↔GPU mode switches reuse it).
- The `Ort::Session` is **created** (in `makeSession`) **and** `Run()` is called (in `runOneModel`) **on the SAME thread** — the GentSampler **worker thread** (`GentSamplerAudioProcessor::run()` override → `doStemJob` → `separate` → `runModelFile` → `makeSession` then `runOneModel`).
- **Therefore there is NO session-create/Run thread mismatch.** `cudaSetDevice(0)` is already called on that worker thread before session creation. **The "same-thread + cudaSetDevice" hypothesis is fully addressed and did NOT fix the crash.** The remaining fault is the in-FL-process crash inside `cudnn64_8.dll` / `onnxruntime_providers_cuda.dll` (Section 3).
- Two *untested* threading angles remain (Section 7): (a) the worker thread also does CPU work + reuses a **CPU-created `Ort::Env`** for GPU; (b) a brand-new dedicated GPU-only thread + a **fresh Env** has not been tried.

---

## 6. Deferred / not wired (all GPU-only → moot while CUDA is gated off)
- **fp16 manifest selection in C++** `parseManifest` — currently always uses `onnx_files` (fp32); `onnx_files_fp16` is parsed-ready but ignored. Wire: pick fp16 when `gpuActive`.
- **First-run download wiring for the GPU pack** (CUDA/cuDNN DLLs) + fp16 weights — needs real sizes + SHA-256 once hosted. (The fp32 CPU model download is done.)
- **MAX QUALITY UI toggle** — processor flag `stemMaxQuality` + `canUseMaxQuality()` gating exist; no visible UI control yet (test via `separate_maxq.txt`).

---

## 7. FORK-FORWARD PLAN — ordered debugging checklist for the in-host crash

The blocker is the **in-FL-process crash** in `cudnn64_8.dll` (0xC0000409) / `onnxruntime_providers_cuda.dll` (0xC0000005). The model + GPU are proven good (Python). **The thread-mismatch + `cudaSetDevice` fix is already done — do not re-do it.** Start here:

1. **Get a real call stack (highest value).** Load the plugin in a host **you can attach a debugger to** — JUCE `AudioPluginHost`, `pluginval`, or a tiny custom VST3 host — set `kEnableCuda=true`, rebuild, trigger GPU separation under Visual Studio/WinDbg. The 0xC0000409 / 0xC0000005 will break into a stack that names the exact function. This turns the mystery into a known fault. (Repro outside FL also tells you if it's FL-specific.)
2. **Test the Control-Flow-Guard / mitigation hypothesis.** `0xC0000409 = STATUS_STACK_BUFFER_OVERRUN` is the CFG/`__fastfail` signature. Run `Get-ProcessMitigation -Name FL64.exe`. As a TEST only, `Set-ProcessMitigation -Name FL64.exe -Disable CFG` (or test in a host without CFG). If the crash vanishes → it's a mitigation collision; then find a CFG-compatible load path or document the constraint.
3. **Rule out a foreign CUDA context** (FL itself, or an NVIDIA overlay — GeForce Experience / ShadowPlay / Discord). Disable overlays and retest. Consider explicit context management (`cuCtxGetCurrent`/`cuCtxSetCurrent`) on the worker thread.
4. **Run CUDA on a brand-new dedicated thread** that does ONLY `cudaSetDevice → create session → Run → destroy`, isolated from the existing worker thread's prior CPU work and from the reused Env.
5. **Use a FRESH `Ort::Env` for GPU** (don't reuse the CPU-created one) — the Env is currently created once and reused.
6. **Try a different cuDNN 8.9 patch or ORT 1.17/1.19** if the fault is a specific `cudnn64_8.dll` bug at offset `0x1df51`.
7. **If Windows-CUDA stays blocked:** pivot GPU effort to **CoreML on Mac** (Apple Silicon, no FL-Windows-process baggage; the more strategic cross-platform GPU bet). CPU remains the universal path.

**To resume:** set `kEnableCuda = true` in `Source/StemSeparator.cpp`, rebuild, install (mind the gotchas in Section 8).

---

## 8. Build / install / test mechanics (gotchas that bit us)
- **Build:** `cmake --build build --config Release --parallel` from the repo root. Do **not** run `build.bat` directly in automation — its `pause` hangs non-interactively, and its admin-xcopy **silently falls back to `Documents\VST3`**, desyncing from where FL loads.
- **Install path FL actually loads:** `C:\Program Files\Common Files\VST3\GentSampler.vst3\Contents\x86_64-win\` — needs an **elevated** copy. There were repeated **two-location desyncs** (Program Files vs `Documents\VST3`). **Always verify** `engine_check.txt`'s build timestamp == your latest build. Mirror the whole bundle with elevated `robocopy <src> <dst> /MIR` for a clean swap (also deletes stale DLLs).
- **DLLs** must sit next to `onnxruntime.dll` in that folder; the plugin pre-loads them by name.
- **Test hooks:** drop empty files in `Documents\GentSampler\`: `separate_now.txt` (CPU), `separate_gpu.txt` (CUDA standard), `separate_maxq.txt` (CUDA + ft).
- **Diagnostics:** `Documents\GentSampler_engine_check.txt` (ORT version, CUDA EP available, DLL count, at load); `Documents\GentSampler\stems_log.txt` (per-run `took: N s (provider)` + `GPU DIAGNOSTIC:` line); Windows **Event Log → Application → Application Error** for crashes (faulting module).
- **Models:** `Documents\GentSampler\models\` (`manifest.json` + `.onnx`). Full 7.8 s manifest backed up as `manifest.json.bak7s`.
- **Disk:** Joe's `C:` runs near 100 % full. Clean up aggressively: `StemTest/onnx_models_seg*`, any `ort18venv`, and obsolete DLL sets. (The unused cuDNN-8 DLLs now in Program Files are ~1.8 GB and safe to delete while CUDA is gated off.)

---

## 9. Locked facts / decisions (don't relitigate)
- **The model runs on the GPU — PROVEN** (Python, ORT 1.18.1 + cuDNN 8.9.5, full 7.8 s `htdemucs_6s`, RTX 2060). The blocker is **plugin/host integration**, not model or hardware.
- **CPU is the shipping path:** `htdemucs_6s` 7.8 s ≈ 33–38 s, full quality, cross-platform (and the model-level default already gave ~5× over the original 170 s hybrid).
- **fp16 = no audible quality loss** (Joe A/B'd). **ORT/cuDNN version downgrade = zero quality impact** (same `.onnx`, same math).
- **MIT models only** (`htdemucs_ft` + `htdemucs_6s`), hosted at the HF mirror above (fp32; SHA-256 baked into `ModelDownloader.cpp`).
- **Stem order is fixed:** drums, bass, vocals, guitar, piano, other.
- **Target is cross-platform** (Windows + macOS/Logic). On Mac the GPU path is **CoreML**, not CUDA — so Windows-CUDA is only half the GPU story and the fragile half.
- **Non-GPU product is feature-complete:** pan, choke groups, per-pad filter, loop/reverse, inspector cue-range + routing, full mockup UI, working first-run model downloader — all done and verified in FL.
