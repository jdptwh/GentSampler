# BACKLOG — future specs, not scheduled

Items here need a spec before any implementation. Do not pick these up
without Joe green-lighting a spec.

## macOS PORT — future project (Joe-requested 2026-07-07 at installer approval)
GentSampler is Windows-x64-only today. A Mac build is feasible (JUCE is
cross-platform; AU + VST3 targets) but is a real porting project, not a
packaging task: Xcode/clang toolchain; macOS ONNX Runtime binaries (the
current manual LoadLibraryExW loading in StemSeparator.cpp is Windows-
specific -> dlopen or direct-link path needed); universal Intel/Apple-Silicon
binaries; Apple code-signing + NOTARIZATION (mandatory — unsigned plugins
effectively don't run on modern macOS; Apple Developer account required);
Direct2D-specific rendering landmines re-audited under CoreGraphics/Metal;
first-run model download path re-verified; full re-validation (pluginval mac
+ a mac DAW). Needs its own spec + Joe's Apple-account/business decisions
before any work.

## RESOLVED 2026-07-07 (TEARDOWN_FIX_SPEC.md, 5a66bc1, Joe-verified) — was: HIGH — FL hangs on close with GentSampler loaded
Repro (Joe): open any FL project containing GentSampler -> close FL -> FL goes
unresponsive, requires force-close. Happens EVERY time, ONLY when GentSampler
is in the open project; has occurred "all throughout the build."
PRIME SUSPECT (initial audit 2026-07-07, unconfirmed): Theme.h:325
`glowSprite()` holds a function-local `static juce::Image` (64x64 ARGB).
Function-local statics destruct at DLL_PROCESS_DETACH (FL FreeLibrarys the
plugin at close) UNDER THE WINDOWS LOADER LOCK; per this repo's own 0.4
finding, default `juce::Image(ARGB,...)` routes through the NATIVE/Direct2D
image type in this JUCE 8 setup -> its destructor releases D2D/DXGI COM
resources under loader lock -> classic deadlock. Timeline fits (glow sprite =
Phase A skin). Painted by every arc knob + the BLEED slider, so it is created
in every editor session.
Other teardown paths audited clean so far: processor dtor = cancelPendingUpdate
+ stopThread(10000) (worker idles in wait(250), exits fast); Ort::Env objects
are per-instance unique_ptrs destroyed at processor dtor (not statics);
onnxruntime.dll is LoadLibrary'd and intentionally never freed (safe);
gentCheckStemEngine probe creates no Env/threads.
CONFIRMATION WANTED before fixing: (a) hang duration (forever vs ~10s bounds
the mechanism); (b) does the STANDALONE also hang on close? (standalone exits
via ExitProcess -> statics may not run the same way -> a no-hang there is
consistent with the loader-lock theory); (c) ideally a hang dump (Task Manager
-> Details -> FL64.exe -> Create dump file while hung) for definitive stacks.
FIX CANDIDATE (cheap, zero-risk): intentionally leak the sprite
(`static juce::Image* img = new juce::Image(...)`) so no destructor runs at
DLL detach — the standard JUCE-plugin pattern for native-resource statics;
16KB reclaimed by the OS at process exit. Audit for any sibling static JUCE
objects while in there. Needs a micro-spec + Joe approval.
RESOLUTION: dump-proven (FL64.DMP: LdrUnloadDll -> static dtors -> d3d11 -> nvwgf2umx infinite wait under loader lock); glowSprite static leaked intentionally; Joe verified clean FL closes. INSTALLER pause lifted.

## P3 retry: per-column wave-gradient "breathing" (benched 2026-07-02)
REDESIGN_C6_POLISH.md P3 was reverted after the cached 1x256 waveRamp blit
rendered the hero wave at ~half alpha (measured lum 51->27, amber lost).
Suspected but UNCONFIRMED mechanism: 1px-wide source image edge-bleed under
effectively-bilinear resampling. Any retry must (a) reproduce+confirm the
actual alpha-loss mechanism first in isolation, (b) keep the zero-per-paint-
allocation constraint, (c) pass the numeric bar: hero wave band luminance in
[43,60] with red>green by >=8, measured via the scratchpad capture rig, and
(d) compare against the mockup's per-column drawComposite. Alternatives if
the blit is unfixable: 3-segment bright-cap/dim-core vertical lines, or a
256-entry precomputed Colour ramp with per-column drawVerticalLine spans.
SEV-LOW nicety — never let it block a phase again.

### Retry attempt 2026-07-03 (Phase 3 task 0.4) — SKIPPED, mechanism UNCONFIRMED (contradicted)
Ran the isolation experiment per PHASE3_SPEC.md 0.4 in a scratch JUCE console/GUI
harness (own CMakeLists, `add_subdirectory` reusing the already-fetched
`build/_deps/juce-src`; zero production diff — repo `git status --short` clean
throughout, confirmed after). Stand-up cost ~20-25 min total, inside the ~30 min
budget, but only after two real obstacles: (1) MSVC's FileTracker fails
(`FTK1011`) building anything under the deep default Temp scratchpad path — had
to relocate the scratch build to a short path (`C:/gswave`, deleted after — see
note) to get a configure at all; (2) the default `Image(Image::ARGB,...)`
constructor routes through the native (Direct2D) image type off-screen, which
silently no-ops all drawing (confirmed via a `fillRect` sanity check returning
all-zero pixels) — required `SoftwareImageType()` explicitly for the
console-harness variant, and a second, more faithful `Component` +
`createComponentSnapshot()` harness (a real native peer, same backend class the
plugin editor's `paint()` actually uses) to be sure the off-screen substitution
wasn't itself the confound.

Four variants measured on the peer-backed harness (quiet-passage amplitude,
40x120 test strip, band = hero-wave-height rows around mid, composited over
black, Rec.709 luminance):
- (a) 1x256 source blit -> 1px dest column, default resampling (the shipped/reverted mechanism): lum=72.37, R-G=34.92
- (c) same, explicit `lowResamplingQuality`: lum=72.39, R-G=35.00
- (b) 4x256 source, interior 2px source rect (duplicated-edge-column variant): lum=72.37, R-G=34.92
- (d) direct per-column `ColourGradient` fill, no image blit at all (truth reference): lum=72.31, R-G=35.00

All four agree within 0.08 luminance and 0.08 R-G — no measurable alpha-loss or
edge-bleed reproduces in isolation; the 1px-source blit is statistically
identical to the truth reference. This means the previously suspected
mechanism (1px-wide source edge-bleed under bilinear resampling) does NOT
reproduce the shipped regression (lum 51->27) at all in a faithful native-peer
paint context. Per spec skip rule ("mechanism not confirmed in the rig
session"), retry SKIPPED — Phase B not attempted, no port to
`Source/PluginEditor.h`.

What remains unknown: the real production regression's actual cause is still
unexplained. Candidates not ruled out by this experiment: (i) something in the
*real* composite loop's surrounding state (clip region, alpha-compositing layer
from an enclosing `Component::paintEntireComponent` opacity, or a parent-level
`setOpaque`/off-screen buffer) interacting with the blit in a way this isolated
strip doesn't reproduce; (ii) the original bug may not have been the blit
mechanism at all but something else changed in the same diff (e.g. resampling
quality state leaking from a prior draw call, or the gradient's own stop values
being different from what REDESIGN_C6_POLISH.md's snippet specifies). A future
attempt should instrument the actual `WaveformView::paint()` composite loop
in-place (behind a feature flag, easily revertable) rather than an isolated
strip, since the isolated mechanism cannot be reproduced standalone.

Housekeeping note: the harness needed a short build path (`C:/gswave`) outside
both the repo and the sanctioned scratchpad because the scratchpad's deep Temp
path breaks MSVC FileTracker; that directory was to be deleted after the
session but the tool sandbox denied `rm -rf` outside the working directory —
`C:/gswave` may still exist on disk and is safe to delete manually (build
artifacts only, no repo content).

## RESOLVED 2026-07-05 (`2ee2f53`, DATA_INTEGRITY_SPEC.md) — was: HIGH — synchronous loadFile-on-construct in state restore (Pad12-ghost family) (found 2026-07-04)
`applyStateTree()` (PluginProcessor.cpp:2594-2599) restores the persisted source
by calling `loadFile(f, false)` **synchronously on the calling thread** whenever
the stored `path` still `existsAsFile()`. That path runs from
`setStateInformation()` (cpp:3207-3218) — i.e. on the host's message thread at
project reopen / preset recall / plugin re-instantiation. Two problems, both
data-integrity / host-stability:
1. **Blocking decode on the message thread.** A large source file decodes inline
   during host state restore (no worker handoff), stalling the host for the
   decode duration on every reopen. This is the same *class* of construct-path
   blocking as the CUDA-preload hang just fixed, on a different resource.
2. **Ghost / silent-empty restore (the recurring bite).** This is the family
   behind the two known landmines — the standalone persisting the last-loaded
   file and slice-export artifacts (e.g. `GentSampler_Pad12.wav`) restoring as a
   silent EMPTY source (CLAUDE.md, 2026-07-02). If the stored path now points at
   a stale/overwritten/empty export, restore silently loads garbage and it reads
   as a paint/render bug. Also related: the async-clobber-on-reload item (below /
   queued) — restore order vs. worker-thread slice rebuilds can drop slice edits.
Not scoped here. Spec should cover: worker-thread deferral of the restore decode
(mirror `analysisThenSlice`/`wantRender` deferral), validating the restored
source is non-empty before adopting it, and reconciling restore vs. the
async slice-rebuild path. SEV-HIGH — silent data loss / host stall on a
core round-trip. Needs a spec before implementation.

## RESOLVED 2026-07-07 (PACKAGING_SPEC.md P1/P2) — was: PACKAGING — do not ship the ~2.6 GB CUDA/cuDNN pack
The CUDA/cuDNN dev pack was purged from BOTH the artefact-bundle source
(P1, 12 files) and the deployed Program Files folder (P2, 12 files / 2.07 GB,
Joe-approved dry-run). Nothing regenerates it (POST_BUILD copies only the 2
core ORT DLLs; the deploy step copies only the binary; ModelDownloader has NO
CUDA download code — the old "first-run CUDA fetch" description was a stale
design note, corrected in CMakeLists comments + THIRD_PARTY_LICENSES this
pass). build.bat's robocopy `/xf` exclusion (WAVE4 F5) guards the install
path. Clean-machine proxy validated (PACKAGING_SPEC P3).

## RESOLVED 2026-07-07 (INSTALLER_SPEC.md T1-T5, Joe clean-machine PASS) — was: INSTALLER — follow-on spec
Inno Setup 6 installer built (payload = VST3 + 2 core ORT DLLs + license
texts, no Standalone, no CUDA), version-parsed and fail-closed
(`installer\make_installer.bat` / `installer\GentSampler.iss`). Dev-box
install/uninstall/restore cycle PASS (T4). Joe's separate clean machine ran
`INSTALLER_TEST_CHECKLIST.md` with no toolchain: install, FL load, one CPU
separation (first-run ~1.79 GB model download) all confirmed working — T5
PASS 2026-07-07 ("seems like we are all good"), closing PACKAGING P3's
deferred no-toolchain/FL-round-trip/model-download rows per OQ-INST-5 scope.

## BEFORE PUBLIC RELEASE — three remaining gates
Prerequisites and the installer itself are DONE. Public distribution of the
installer still needs, in no particular order:
1. JUCE commercial license PURCHASE — mode declared 2026-07-07 (commercial
   intent, recorded in THIRD_PARTY_LICENSES); the actual purchase/activation
   is the blocker. No splash/AGPL work needed once purchased.
2. Code-signing certificate — the installer exe is unsigned today, which
   trips SmartScreen ("More info -> Run anyway"); cert cost/process is a Joe
   business decision.
3. EULA decision — no license text ships with the installer today
   (deliberate, per PACKAGING OQ-PKG-C precedent: no invented legal text);
   whether GentSampler needs one is still an open decision.

## RESOLVED 2026-07-05 (`2ee2f53`, DATA_INTEGRITY_SPEC.md) — was: HIGH — async clobber on project reopen silently drops slice edits (filed 2026-07-04)
Data-integrity race on the state-restore path (paired with the sync-loadFile
item above — same reopen path, fix them together). `setStateInformation()` ->
`applyStateTree()` (PluginProcessor.cpp:2585-2636) restores `cues[]`/`cueEnds[]`/
`padStemMask[]` from saved state, reloads the source via `loadFile(f,false)`
(runAnalysis=false, cpp:2594-2599), then finishes with `wantRender = true;
notify();` (cpp:2633-2634). That wakes the worker `run()` loop, which on the
same signal can run `doAnalysisJob` / `doRenderJob` / `doPadRenderJobs`
(cpp:817-823). Concern: worker rebuild work fired by (or racing with) restore
can overwrite the just-restored cues with freshly-derived transient slices, so a
reopened project silently loses hand-edited slice boundaries — no error, reads
as "my slices reset themselves."
Needs a spec to: (1) trace the exact ordering of restore vs. every `want*`
worker job and prove which can re-derive cues after restore; (2) make restore
authoritative — rebuild jobs must NOT clobber restored cue/end/mask state (a
restore-generation guard, or defer/sequence restore so no re-slice fires on
load, only on explicit user action); (3) add a pure-logic regression where
possible (restore-then-rebuild ordering) plus a manual reopen check. SEV-HIGH —
silent loss of user slice edits on a core save/reopen round-trip. Do not pick up
without Joe green-lighting a spec.

## Verification-surface gap: concurrency/lifecycle properties untestable (filed 2026-07-05, WAVE1 close)
Per ROUTING.md Rule 2 ("an inadequate verification surface is a DEFECT, not a
steady state"): WAVE1 fixes F2/F4/F5/F6 (restore races, multi-instance ORT
init, cross-process download lock, editor-lifecycle use-after-free) have NO
automatable regression coverage — the ctest harness is logic-only and cannot
drive two plugin instances, host state-restore sequences, or destroy-editor-
while-dialog-open. Accepted for WAVE1 with reviewer diff-reads + Joe manual
repros as the gate. Candidate spec: a minimal host-harness test target
(instantiate 2 processors, drive setStateInformation twice, open/close
editors) — needs a spec before implementation.

## v2 classifier: finish_classification demotion uses caller preset, not activeT (found 2026-07-05, WAVE3 #18)
While writing 1c-fallthrough tests (WAVE3 #18, tests/ClassifierTests.cpp —
see the OPEN QUESTION block), the implementer PROVED the demotion rule
(winningConfidence < t.minConfidence -> OTHER) reads the CALLER's preset
`t` (kThreshStems, 0.50) instead of `activeT` (kThreshNoStems), the preset
that actually ran the fallthrough's spectral tree — and the no-stems tree's
confidence floor is exactly 0.5, so demotion can NEVER fire through 1c.
Likely intent: activeT.minConfidence. PARKED with the v2 classifier (dev
report only in v1; PHASE3_SPEC.md is the v2 record); fix + re-baseline the
affected tests at the v2 ear-gate, not before.
