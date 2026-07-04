# BACKLOG — future specs, not scheduled

Items here need a spec before any implementation. Do not pick these up
without Joe green-lighting a spec.

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

## HIGH — synchronous loadFile-on-construct in state restore (Pad12-ghost family) (found 2026-07-04)
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

## PACKAGING — do not ship the ~2.6 GB CUDA/cuDNN pack in the release build (flagged 2026-07-04)
The full CUDA 11.8 / cuDNN 8.9 runtime (~2.6 GB: `onnxruntime_providers_cuda.dll`
410 MB, `cudnn_cnn_infer64_8.dll` 571 MB, `cublasLt64_11.dll` 544 MB,
`cufft64_10.dll` 280 MB, plus the rest of `kCudaDlls`) currently sits beside the
build artefact (`build/GentSampler_artefacts/Release/VST3/.../x86_64-win/`,
dated 2026-06-25). Per CLAUDE.md/CMakeLists the shipping design is: bundle only
the two small core ORT DLLs next to the VST3; the CUDA EP + cuDNN + weights are a
first-run ModelDownloader fetch to keep CPU-only installs small. The GPU pack is
dev-box leftover from GPU experiments, not a build output (POST_BUILD copies only
the 2 core DLLs). Runtime is CPU-only (`kEnableCuda=false`), and construct no
longer touches these (fixed 2026-07-04). For the eventual installer/packaging
pass: ensure the release/installer excludes the CUDA pack entirely (CPU-only
payload), and confirm the pluginval/CI artefact folder doesn't carry it either
(it was the trigger for the cold-open hang). No runtime dependency on these DLLs
exists while CUDA is shelved. Packaging concern only — no code change implied.
