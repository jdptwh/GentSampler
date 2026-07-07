# GentSampler Installer Test — Clean Machine Checklist

You need exactly two things on the test machine: **the installer exe** and
**FL Studio**. No git, no CMake, no Visual Studio, no build tools — that is
the point of this test.

The exe: `GentSamplerSetup-1.1.0.exe` (copy it from the dev box:
`C:\Users\JoeyD\Desktop\GentSampler\GentSampler\dist\`). USB or a network
share both fine.

## Steps

1. **Copy the exe over and double-click it.**
   - If Windows SmartScreen shows "Windows protected your PC": click
     **More info → Run anyway**. This is expected — the exe is unsigned
     (signing is a before-public-release item, not needed for this private
     test).
   - A UAC prompt appears (the installer needs admin to reach the shared
     VST3 folder) → **Yes**.
2. **Step through the wizard.** It should be ≤5 clicks with **no choice of
   install directory** (the VST3 location is standard-mandated). No error
   dialogs expected.
3. **Verify the install.** Open Command Prompt and run:
   ```
   dir "C:\Program Files\Common Files\VST3\GentSampler.vst3\Contents\x86_64-win"
   ```
   Expect EXACTLY three files: `GentSampler.vst3`, `onnxruntime.dll`,
   `onnxruntime_providers_shared.dll` — and no `cublas`/`cudnn`/`cufft`/
   `curand`/`nvrtc`/`cudart` names anywhere. Confirm the path really says
   `Program Files`, **not** `Program Files (x86)`.
   **Copy/photo this listing for the report.**
4. **FL Studio:** open FL → Options → Manage plugins → "Find more plugins"
   (a rescan). GentSampler should appear under Installed → Generators →
   VST3 **without adding any search paths**. Load it on a channel, drop a
   track in, hit some pads.
5. **One stem separation** (SEPARATE STEMS button). First use downloads
   ~1.79 GB of models to `Documents\GentSampler\models` — it needs
   internet and takes a while depending on connection. Let it finish, then
   confirm the stems play (STEMS view / DRM-BAS-VOX chips).
6. **Close FL** with the project open — it should exit cleanly (this
   re-verifies the teardown fix on a second machine).
7. **Optional:** uninstall via Settings → Apps (or Add/Remove Programs) →
   "GentSampler 1.1.0" → confirm the folder
   `C:\Program Files\Common Files\VST3\GentSampler.vst3` is completely gone
   and `Documents\GentSampler` (your models) is untouched. Reinstall after
   if you want to keep it.

## Report back

- SmartScreen seen? (y/n) — UAC prompt seen? (y/n)
- Any errors, verbatim
- The step-3 `dir` listing (text or photo)
- FL found + loaded the plugin without path fiddling? (y/n)
- Separation completed incl. model download? (y/n)
- FL closed cleanly? (y/n)
- (If tried) uninstall left nothing behind / models intact? (y/n)
