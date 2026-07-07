# Clean-Machine Checklist (Joe's separate box)

Follow in order on a Windows machine that never had GentSampler or its dev
tools installed. Nothing needs to be preinstalled.

## 1. Install prerequisites
Open **PowerShell** and run:
```powershell
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```
Close and reopen PowerShell after these finish so PATH refreshes.

## 2. Get the repo onto the machine
This repo has no GitHub remote (local-only) — copy the folder over (USB,
network share, or zip) rather than `git clone`ing a URL; git history isn't
needed, just the files. **Use a SHORT path**, e.g. `C:\gs` or `D:\gs` — do
NOT nest it several folders deep (long paths can break the build tools
mid-compile).

## 3. Build
Double-click **build.bat** (right-click → "Run as administrator" to install
system-wide; otherwise it falls back to Documents — see step 4).

**Expected:**
- First run downloads JUCE, ONNX Runtime, and the stretch engine (a few
  minutes, needs internet), then "Building Release..." (several minutes).
- No message about a "deploy step" — that's an internal dev-only step that
  only fires on the original dev machine and is silently skipped here.
  Normal, not an error.
- Last line is either `[OK] Installed to: C:\Program Files\Common Files\VST3`
  (admin) or `[OK] Installed to: <Documents>\VST3` (no admin — note this,
  you'll add that folder as a search path in FL, step 6).
- Press any key to close.

If "Configure failed": Build Tools didn't finish installing, or PATH hasn't
refreshed — reopen the window, re-run step 1's third command, retry.

## 4. Verify the install (no CUDA files)
Open `C:\Program Files\Common Files\VST3\GentSampler.vst3\Contents\x86_64-win`
(or `Documents\VST3\...`). **Expected: exactly 3 files** — `GentSampler.vst3`,
`onnxruntime.dll`, `onnxruntime_providers_shared.dll`. NO files named
`cublas*`, `cudart*`, `cudnn*`, `cufft*`, `curand*`, `nvrtc*`, or
`onnxruntime_providers_cuda.dll`. If any of those are present, stop and
report back.

## 5. Optional: pluginval
`winget install pluginval`, then:
```powershell
pluginval --strictness-level 5 --skip-gui-tests --validate "C:\Program Files\Common Files\VST3\GentSampler.vst3"
```
Expected: ends with `SUCCESS`. Skip this if you'd rather go straight to FL.

## 6. FL Studio smoke test
1. Options → Manage plugins → Find more plugins. If installed to
   Documents\VST3, first add that folder under Options → File settings →
   VST plugins search paths, then rescan.
2. GentSampler appears under Installed → Generators → VST3 — drop it on a
   channel.
3. Drag an audio track into the plugin window; hit a few pads (confirm sound).
4. Try one CPU stem separation. **First use downloads the separation model
   (~1.79 GB) — needs internet, will take a while.** After that it's local.

## 7. Report back
- Whether build.bat completed and which install path it used.
- The file listing from step 4 (should be exactly the 3 files above).
- pluginval result if you ran it (pass/fail).
- Whether FL loaded the plugin, played pads, and completed one separation.
- Any error message, verbatim, if something didn't work.
