# SPEC: Redesign Phase C6 — Polish (three flags, nothing else)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ HEAD `041cc5d`
**Origin:** Joe confirmed exactly three flags from the C5 gate review, 2026-07-02. No new features.

## Goal

Close the exactly three visual flags Joe confirmed from the C5 gate review — transient-tick density on long slices, cue-region glow pallor, and per-column wave-gradient breathing — as paint-only changes, then commit. Nothing else changes.

## Ground rules

1. **Paint-only.** Zero behavior, interaction, layout, geometry, hit-zone, timer, or member-layout changes. All edits live inside `paint()` bodies (plus one new cached-asset accessor in Theme.h).
2. **PluginProcessor.h / PluginProcessor.cpp are untouchable.** Any edit there is an automatic FAIL.
3. **The mockup is the spec** for items 2 and 3: `C:\Users\JoeyD\Downloads\gentsampler_redesign_v2.html` (`drawComposite` lines ~453-467, `drawFlags` lines ~434-451, detail strip IIFE lines ~502-519).
4. **Scope discipline is the deliverable.** If you encounter anything else that looks wrong — a bug, a style inconsistency, a tempting refactor — REPORT it in your completion notes and do not touch it. Fixing it is a spec violation even if the fix is correct.
5. No new dependencies. Only the files in the file plan change.
6. Perf floor: both surfaces repaint at 30 Hz (`startTimerHz (30)`, PluginEditor.h:235 and :1177). No per-paint heap allocations beyond what the existing code already does. Specifically forbidden: constructing `juce::ColourGradient` per column.

## File plan

| File | Change |
|---|---|
| `Source/Theme.h` | ADD one inline cached-image accessor `waveRamp()` next to `glowSprite()` (~line 333). No existing token or function is edited. |
| `Source/PluginEditor.h` | MODIFY three paint blocks only: `WaveformView::paint` composite wave loop (lines ~443-458), `WaveformView::paint` cue-region block (line ~509, one-line deletion), `SliceDetailStrip::paint` tick block (lines ~1273-1280). |
| `CLAUDE.md` | UPDATE "Current state" after PASS (C6 done, committed). |

Nothing else. Not `build/`, not the processor, not `resized()`/layout, not mouse handlers.

---

## P1 — Transient-tick density cap (SliceDetailStrip)

**Current code** (PluginEditor.h:1273-1280): every onset in `[zoomLo, zoomHi]` draws a 1.5px × (0.16·h) white rect at alpha 0.22. On an 84 s slice the ~700px wave area (`waveR - waveL`) gets hundreds of ticks → solid barcode.

**Mechanism (both spacing cap and density fade):**

Replace the tick loop with:

1. **Pre-count pass:** iterate `onsets` once counting `nVis` = onsets with `zoomLo <= t <= zoomHi`. (Onsets arrive time-ordered from the Analyzer via `getOnsetPositions()` — PluginProcessor.cpp:565-572 copies `transientOnsets` in stored order. If the implementer finds them unsorted in practice, sort the local copy once in `timerCallback` where `onsets` is refreshed (line 1518) — never per paint.)
2. **Density alpha:** `avgSpacing = (float)(waveR - waveL) / (float) jmax (1, nVis)`; `tickAlpha = juce::jmap (juce::jlimit (6.0f, 24.0f, avgSpacing), 6.0f, 24.0f, 0.10f, 0.22f)`.
3. **Draw pass with min-spacing skip:** track `lastDrawnX` (init `-1.0e9f`); draw a tick only when `tx - lastDrawnX >= 6.0f`, then set `lastDrawnX = tx`. Tick geometry and colour family unchanged: `fillRect (tx - 0.75f, h*0.06f, 1.5f, h*0.16f)`, white at `tickAlpha`.

**Acceptance criteria:**
- **P1.1** On a typical sub-10 s slice where onsets are ≥24 px apart, rendering is pixel-identical to today (alpha 0.22, zero ticks skipped — verify by eye on a normal drum slice).
- **P1.2** On the C5 long-slice repro (pad 1 = full ~84 s track), drawn ticks number ≤ `(waveR-waveL)/6` (~117 at default width), at alpha 0.10; individual ticks are visually separable (no solid band) in the screenshot.
- **P1.3** No change outside the tick block: wave columns, dim overlays, handles, grain marker, playhead, readouts all byte-identical code.

## P2 — Cue-region glow warmth (WaveformView hero)

**Diagnosis (from code + mockup comparison — this is the prescribed cause, not a guess):** mockup `drawFlags` lines 444-445 draws the active cue region as fill `rgba(232,153,58,.10)` + `strokeRect rgba(255,184,92,.65)` lw 2 — **and nothing else; there is no shadow/bloom on this rect** (the only soft pass nearby is the playhead's own linear gradient, lines 447-449). The build (PluginEditor.h:507-511) matches fill and border exactly but inserts `Theme::featherGlow (g, region, 0.0f, Theme::glow.withAlpha (0.35f), 5.0f, 3)` between them. `featherGlow` (Theme.h:97-109) strokes its innermost layer at full input alpha (0.35) with a ~2.5px stroke centered ~0.8px outside the region edge — it overlaps the border band, compositing #FFB85C toward ~0.77+ effective alpha (near-opaque #FFB85C is a pale cream), and paints an outward pale halo the mockup never had. That is the pallor.

**Fix:** delete line 509 (`Theme::featherGlow (...)`) — one line. Fill (accent @ 0.10, line 507-508) and border (glow @ 0.65, 2px, `region.reduced (1.0f)`, lines 510-511) stay byte-identical, matching mockup tokens 1:1. Do not compensate by darkening the border or changing tokens; if the re-capture still reads pale next to the mockup after removal, STOP and report — do not improvise a colour change.

**Acceptance criteria:**
- **P2.1** `featherGlow` call removed; the cue-region block contains exactly: fill `Theme::accent.withAlpha (0.10f)` + `drawRect (region.reduced (1.0f), 2.0f)` in `Theme::glow.withAlpha (0.65f)`.
- **P2.2** Re-captured hero side-by-side vs mockup: region border reads as the same amber depth as the mockup's (no whitish halo outside the border). Reference for "before": `C:\Users\JoeyD\Desktop\PhaseC_sbs_hero.png`.
- **P2.3** Draw order unchanged (wave → cue region → flags/handles, per the C2 comment block at lines 491-497) — the C2 rationale comment is updated to drop the featherGlow mention, nothing else.

## P3 — Per-column wave-gradient breathing (WaveformView hero composite)

**Diagnosis:** mockup `drawComposite` (HTML 458-463) creates its gradient **per column**, stretched `mid-a → mid+a` over that column's own amplitude, stops `0:.85 / .5:.28 / 1:.70` of accent #E8993A — so every column, however quiet, shows bright edges at its own extremes. Build (PluginEditor.h:445-458) sets one brush over fixed `mid ± half` (half = 0.48 band height); quiet columns sample only the dim 0.28 core. Confirmed C1 SEV-LOW, now in scope.

**Mechanism — cached ramp-image blit (chosen over 3-segment caps):**

1. **Theme.h:** add `inline const juce::Image& waveRamp()` beside `glowSprite()` (same function-local-static pattern, Theme.h:333-343): a 1×256 ARGB image built once by filling with a `juce::ColourGradient` of `accent.withAlpha (0.85f)` at y=0, added stop `0.5 → accent.withAlpha (0.28f)`, `accent.withAlpha (0.70f)` at y=255.
2. **PluginEditor.h composite loop (replaces lines 447-458):** drop the gradient brush entirely. Per column:
   ```cpp
   const float y0 = mid - pk.second * half, y1 = mid - pk.first * half;   // top, bottom
   g.drawImage (Theme::waveRamp(),
                x, juce::roundToInt (y0), 1, juce::jmax (1, juce::roundToInt (y1) - juce::roundToInt (y0)),
                0, 0, 1, 256);
   ```
   Wrap the loop in a `juce::Graphics::ScopedSaveState` and set `g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality)` inside it (state restore puts quality back for the rest of paint). The 0.5 stop lands at each column's own midpoint (the build's columns are true min/max, slightly asymmetric about the axis — the mockup's are synthetic-symmetric, so column-midpoint is the faithful mapping of its formula).

**Justification vs alternatives (perf constraint):** per-column `ColourGradient` = ~1000 heap allocations per frame at 30 Hz — forbidden. Three `drawVerticalLine` caps/core per column = same order of draw calls but a stepped approximation with visible cap/core seams. The cached blit touches the same pixel count the current gradient fill already touches, allocates nothing after first call (the `glowSprite()` precedent), and reproduces the mockup's ramp exactly.

**Strip decision — NO change:** the mockup's detail canvas (HTML 512-518) draws flat fills (`'#4D9DE0'` @ .9 in-slice, `'#252B33'` @ .5 outside) with no gradient — it does not breathe. The build's strip (PluginEditor.h:1266-1272) is already structurally faithful (flat hue fills). The mockup is the spec, so `SliceDetailStrip`'s wave loop is inside the regression fence, not the work.

**Acceptance criteria:**
- **P3.1** `Theme::waveRamp()` exists, is built exactly once (function-local static), 1×256, stops .85/.28/.70 of `Theme::accent`.
- **P3.2** Composite loop contains no `juce::ColourGradient` construction and no allocation; one `drawImage` per column; resampling quality scoped with `ScopedSaveState`.
- **P3.3** Re-captured hero vs mockup: quiet passages show bright top/bottom edges at their own column extents (the "breathing" texture), matching the mockup's composite; loud passages look unchanged in character.
- **P3.4** Stem-lane ribbons (lines 387-397), snap grid, flags, playhead, empty-state, and the entire `SliceDetailStrip` wave loop are byte-identical code.
- **P3.5** No perceptible paint sluggishness in the standalone at default window size while a pad plays (subjective floor: playhead motion stays smooth, per Joe's eyeball — the machine proxy is that pluginval's editor stress passes as before).

## UI acceptance criteria

(JUCE editor — pluginval editor tests + reviewer/Joe inspection stand in for Playwright.)

- Hero and strip render without artifacts at 1040×700 and at the 880×592 floor (resize the standalone and eyeball both surfaces — ramp blit and tick thinning are width-relative, nothing hardcoded to 1040).
- All Phase C + feel-task interactions still work untouched: handle grab/drag on both surfaces, Shift-fine, Alt-snap-bypass, arrow nudge ring, stem mute/solo lanes, grain marker drag. (Spot-check only — the diff must make regression structurally impossible by never leaving `paint()`.)

## Design source

`C:\Users\JoeyD\Downloads\gentsampler_redesign_v2.html` — `drawComposite` / `drawFlags` / detail-strip IIFE are the visual ground truth for P2/P3. `C:\Users\JoeyD\Desktop\PhaseC_sbs_hero.png` is the "before" reference. Deviations require a spec change, not a judgment call.

## Verification commands

```
cmake --build build --config Release --parallel
bash .claude/hooks/gate.sh   # build + pluginval strictness 5
```

**Gate 3 — visual re-capture** (reuses the C5 rig in the session scratchpad):
1. Launch `build\GentSampler_artefacts\Release\Standalone\GentSampler.exe`, load a full track, let analysis finish.
2. Capture with the scratchpad `wincap.py` + `find_plugin_rect.py`.
3. Produce: `sbsC6_hero.png` (build hero vs mockup composite — checks P2.2, P3.3) and `sbsC6_strip_long.png` (strip with the ~84 s pad-1 slice selected — checks P1.2).

## Regression fence (out of scope — explicit)

- `Source/PluginProcessor.h` / `.cpp` — untouchable.
- All interaction code: `mouseDown/Drag/Up/Move`, `HandleDragEngine`, hit-test constants (6px/8px), `timerCallback` bodies, hash/dirty logic, `gestureZoomFrozen`.
- All geometry: `bottomChromeInset`, band/lane math, 160px hero, 880×592 floor, `resized()`/layout anywhere.
- Existing Theme.h tokens and functions (`featherGlow` itself stays — only its one call site in the cue region is removed; chips still use it).
- The strip's wave/dim/handle/grain/playhead drawing; the hero's stem lanes, flags, snap grid, empty state, scrollbar band.
- The mockup's `mid = H*.56` vs build's 0.5 wave axis, and the strip's out-of-slice alpha delta (0.28 vs mockup .5) — both are pre-existing accepted approximations, NOT in scope.
- Any other itch: REPORT, don't touch.

## Tier assignment

- **P1 + P2 + P3 implementation: WORK (Sonnet), one task, one diff.** All three are judgment-shaped paint edits with only a compiler behind them (no tests); BULK is barred by ROUTING.md Rule 1. They are small and touch adjacent code — splitting invites merge friction for no routing benefit.
- **Capture rig re-run: same Sonnet task** (mechanical — driving an existing script).
- **Review: one GATE pass over the full diff** after build+pluginval green — verifies fence compliance (paint-only, file list, byte-identical fenced blocks) and the three ACs against the captures.
- **Final acceptance: Joe**, eyeballing `sbsC6_hero.png` / `sbsC6_strip_long.png` (or in FL). His call ends C6.

## Risks

1. **`drawImage` int-rect rounding makes 1-2px columns vanish** → the `jmax (1, ...)` height floor in the P3 snippet guarantees every non-empty column draws at least 1px; reviewer checks quiet-passage columns are present in the capture.
2. **Low-quality resampling of the 1-wide ramp shows stepping on tall columns** → 256 ramp entries vs ≤~115px column height means every dest pixel has a distinct source entry; if stepping still shows, bump the ramp to 512 (still one-time) — that is inside spec, no re-plan needed.
3. **Onsets not time-ordered on some file** → P1 skip logic degrades to drawing too few/many ticks; mitigation is the sanctioned one-time sort in `timerCallback` (P1 mechanism step 1).
4. **P2 removal still reads pale to Joe** → contingency is REPORT + stop (ground rule 4), then a token-level decision at the gate — the implementer must not invent a warmer colour.
5. **Scope creep** — the whole point. The reviewer fails the diff on any hunk outside the three blocks + Theme.h addition, regardless of merit.

## Definition of Done

- [ ] `cmake --build build --config Release --parallel` green
- [ ] pluginval strictness 5 green (installed)
- [ ] P1.1-P1.3, P2.1-P2.3, P3.1-P3.5 checked off
- [ ] `sbsC6_hero.png` + `sbsC6_strip_long.png` captured and attached
- [ ] Diff touches only `Source/Theme.h` + `Source/PluginEditor.h` (three paint blocks)
- [ ] Reviewer PASS (one pass, full diff)
- [ ] Joe accepts the side-by-sides
- [ ] Commit made; CLAUDE.md "Current state" updated (Phase C complete incl. C6 polish)

---

*Spec authored by planner (Fable) 2026-07-02 from Joe's confirmed C6 flags; HEAD at authoring: 041cc5d. Root-cause diagnoses (featherGlow pallor, per-column gradient formula, mockup strip does-not-breathe) verified against code + mockup JS before authoring.*
