# TEARDOWN_FIX_SPEC — FL-close deadlock (planner-authored micro-spec, Rule 8 / Wave-3 precedent; pending Joe approval)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ master tip. 2026-07-07.
**Bug (Joe-reported):** closing FL Studio with any project containing GentSampler leaves FL unresponsive (>1 min, Windows "not responding", force-close required). Every time; only when GentSampler is present; observed throughout the build.

## Diagnosis (dump-proven, not inferred)

Joe captured a hang dump (`FL64.DMP`, 559 MB) while FL was unresponsive; analyzed with cdb
(WinDbg 1.2606.22001.0, installed this session; MS symbol server for OS frames — no
GentSampler PDBs exist in the Release tree, module frames are offsets).

**Main thread (thread 0) — the hang, one stack, bottom to top:**
```
ntdll!LdrUnloadDll                          <- FL unloading GentSampler.vst3 at close
GentSampler+0x1d76f6 / +0x1d75d1            <- DllMain / CRT static-destructor walk
GentSampler!InitDll+0x7a6e9, +0x76268,
GentSampler+0x564f2/0x5678b/0x425b8/0x564af <- our static teardown
d3d11!D3D11CoreCreateDevice+... (multiple)  <- D3D11 device work during that teardown
nvwgf2umx!NVAPI/NVDEV/OpenAdapter...        <- NVIDIA user-mode driver
KERNELBASE!WaitForSingleObjectEx            <- waits forever
```
Static destructors run at `DLL_PROCESS_DETACH` **under the Windows loader lock**. Our
teardown enters Direct3D/the NVIDIA driver, which blocks on a driver-internal
thread/event; that thread cannot make progress while the loader lock is held (thread
exit/attach itself requires it) → permanent deadlock. This is the canonical
never-run-graphics-teardown-in-DllMain failure.

**The static:** `Theme.h:323-339` — `glowSprite()` holds a function-local
`static juce::Image img` (64×64 ARGB). This repo's own Phase-3 finding (BACKLOG,
task 0.4) established that default `juce::Image(ARGB,…)` routes through the NATIVE
(Direct2D) image type in this JUCE 8 setup — so its destructor releases
D2D/D3D11/driver resources. It is the ONLY function-local static JUCE object holding
native resources in the codebase (grep: `static juce::Image|static const juce::Image|
static juce::Font` → one hit). Painted by every arc knob + the BLEED slider → created
in every editor session → matches "every close, all through the build" (sprite dates
to the Phase-A skin).

**Timeline corroboration:** hang is unbounded (>1 min, not the 10 s a `stopThread`
timeout would produce). Other teardown paths audited clean this session: processor
dtor (cancelPendingUpdate + stopThread(10000), worker idles in wait(250)); Ort::Env
per-instance unique_ptrs destroyed at processor dtor; onnxruntime.dll deliberately
never FreeLibrary'd; gentCheckStemEngine probe creates no Env/threads.

## Fix (F1 — one site)

`Theme.h` `glowSprite()`: intentionally leak the sprite so no destructor runs at DLL
detach — the standard plugin-safe pattern for native-resource statics:

```cpp
inline const juce::Image& glowSprite()
{
    // TEARDOWN_FIX_SPEC.md: deliberately LEAKED (heap, never deleted). A
    // function-local static juce::Image is Direct2D-backed here (BACKLOG 0.4
    // finding); its destructor at DLL_PROCESS_DETACH runs under the Windows
    // loader lock and deadlocks inside d3d11/the NVIDIA driver — the FL
    // close-hang proven by the 2026-07-07 FL64.DMP analysis. The OS reclaims
    // the 16 KB at process exit. Never convert back to a by-value static.
    static juce::Image* img = new juce::Image ([] {
        constexpr int N = 64;
        juce::Image m (juce::Image::ARGB, N, N, true);
        juce::Graphics g (m);
        juce::ColourGradient rg (glow.withAlpha (1.0f), N * 0.5f, N * 0.5f,
                                 glow.withAlpha (0.0f), N * 0.5f, 0.0f, true);
        g.setGradientFill (rg);
        g.fillEllipse (0.0f, 0.0f, (float) N, (float) N);
        return m;
    }());
    return *img;
}
```
Behavior-identical at runtime (same lazily-built sprite, same reference returned);
the ONLY change is that the destructor never runs.

## Acceptance criteria
1. The one-site change above lands verbatim (diff = Theme.h only; reviewer diff-read).
2. Gates green: `bash .claude/hooks/build_test.sh` + `bash .claude/hooks/pluginval_gate.sh`
   (pluginval exercises full open/close lifecycles; cold-open timeout = real failure).
3. Grep re-run confirms no other function-local static JUCE object with native
   resources exists (recorded in the commit message).
4. **Joe's gate (the real one):** open FL with a GentSampler project → close FL →
   FL exits cleanly, repeatedly (3+ tries). This is the bug's only true test.
5. If the hang PERSISTS after F1: STOP — do not iterate blind. Capture a fresh dump
   and escalate to a deeper audit (next suspects, in order: JUCE Direct2D factory
   singletons' interaction with FL's unload order; any remaining static with a
   nontrivial dtor). The dump-analysis rig (scratchpad dbg/cdb + symbol cache) is
   set up and documented in this spec for exactly that case.

## Tier / budget / scope
- IMPLEMENTER-scale change but planner-authored and lead-applied (3 lines, complete
  diagnosis attached; Wave-3 Rule-8 precedent). Reviewer diff-read at close.
- Budget: 1 attempt; AC-5 defines the escalation.
- Out of scope: everything else. No other statics changed; no JUCE upgrade; the
  INSTALLER task remains PAUSED until Joe's AC-4 gate is green.

## Record
| Item | Commit | Evidence | Joe |
|---|---|---|---|
| F1 leak glowSprite | (this commit) | diff = Theme.h only; gates green (build+ctest, pluginval strictness 5 incl. open/close lifecycles); sibling-static grep clean (only remaining match is the fix's own comment) | pending close-hang re-test (AC-4, 3+ FL closes) |
