# GentSampler — Phase D1: Stem View State Model

**Date:** 2026-07-03
**Repo HEAD:** `0c04e80` (`Spec: Phase D — COMPOSITE<->STEMS stem map (REDESIGN_TASK_D.md)`)
**Status:** RATIFIED — R1 PASS + Joe sign-off 2026-07-03. Normative for D2-D6.

> This document is normative for D2–D6 per REDESIGN_TASK_D.md's ground rule 1.
> No D2 code exists until this doc has a reviewer PASS and Joe's written
> sign-off below. Deviations from a ratified decision require re-opening D1,
> not an implementer judgment call.

## Sign-off record

| Role | Verdict | Date | Notes |
|---|---|---|---|
| R1 reviewer (Fable) | PASS | 2026-07-03 | Two tightening findings applied in-doc (S6 preamble binary-resolve clarification; DECISION-6 restore-path clause) |
| Joe | SIGNED OFF | 2026-07-03 | All six DECISIONs: recommended defaults ratified, incl. DECISION-6 scope authorization (clear stemSet at the two direct-load call sites only) |

---

## 0. Scope note on citations

Every current-behavior claim below cites a real symbol and file:line **verified
against HEAD `0c04e80`** by direct reading during authorship of this doc (not
copied from the spec's line numbers unread). Two spec-cited locations drifted
slightly from the spec's approximate numbers; both are called out at point of
use with the corrected line. All other spec-cited lines were confirmed exact.

---

## 1. Current-state inventory (code ground truth)

**The lanes band (always-on today, no toggle exists).**

- `Source/PluginEditor.h:301-328` (`WaveformView::paint`) — the always-on stem
  lanes band. Vertical order is *stem lanes → cue flags → waveform*.
  `showStems` (line 302-303) is true if any of the 6 `stemPeaks` arrays is
  non-empty. `stemsShowing = showStems && h > 60` (line 310) is the content
  gate — the `h > 60` clause is a defensive floor against a degenerate
  near-zero layout, not a real cutoff, per the inline C3 comment (lines
  304-309): the hero is fixed at 160px since Phase C, so the old `h > 180`
  gate would have made the band permanently unreachable.
- `bandH` geometry (line 323-324): `bandH = stemsShowing ? jlimit(78, 250, (int)((h - rulerH - 70) * 0.52)) : 0`.
  At the fixed 160px hero this evaluates to the 78px floor. `rulerH = 0`
  (line 316) — the top time-ruler that used to exist above the band was
  deliberately dropped in C3 (comment lines 311-315: the mockup's own
  `drawStems()` has no top ruler in either view, and at 160px there is no
  room for ruler + 6 lanes + flags + any wave sliver).
  `bandTop = rulerH + 2 = 2` (line 317). `flagY = bandTop + bandH + (bandH>0?9:0)`
  (line 326). `top = flagY + flagH + 1` is the waveform/region top (line 327).
  `stemBandTop`/`stemBandH` (member fields, declared line 1136) are set from
  `bandTop`/`bandH` every paint (line 328) and consumed by the hit-tests below.
- Lane paint — mute pill + hover-solo box (lines 356-426, inside `if (bandH > 0)`):
  6 lanes at `laneH = bandH / 6.0f` (line 366). Per lane: a ribbon of the
  stem's peak data between the label pill and the solo box (lines 378-392,
  alpha 0.92 normally / 0.12 when muted); a label pill at the lane's left
  (`labW = stemLabW = 60`, line 367/1139) that is the **mute** toggle,
  filled/tinted by stem colour when unmuted, outlined/struck-through when
  muted (lines 394-410); and a solo "S" box at the lane's right
  (`soloW = stemSoloW = 18`, line 368/1139) that is **hover-revealed**
  (`hoverLane == s`) or shown solid when `soloed` is true (lines 412-424).
- Lane hit-tests and their early-return priority in `mouseDown`
  (`Source/PluginEditor.h:730-740`): the *entire band rectangle* is tested
  FIRST, before the scrollbar (742), middle-pan (752), flag-select (763), and
  edge/handle hit-tests (785+). Any click with `e.y` inside
  `[stemBandTop, stemBandTop+stemBandH)` is fully claimed by the band — lane
  index is `jlimit(0,5,(int)((e.y - stemBandTop) / (stemBandH/6.0)))` (line 732);
  x ≥ `w - stemSoloW - 6` toggles solo, x ≤ `4 + stemLabW + 6` toggles mute
  (lines 733-736); anything else in the band y-range still `return`s at line
  739 and reaches no other hit-test. This whole-band early return is the
  "unreachable slice editing under the band" landmine D4 must dismantle
  (REDESIGN_TASK_D.md landmine flag 3).
- Lane hover in `mouseMove` (`Source/PluginEditor.h:935-945`, spec cited
  942-944 — confirmed exact at 942-944 inside the wider method 935-945): the
  same band-rectangle test recomputes `hl` and repaints only on change
  (line 944); `mouseExit` clears it (947-949).
- `stemPeaks` population: `rebuildStemPeaks()` (`Source/PluginEditor.h:1097-1124`,
  spec cited 1099-1133 — confirmed; the method body is 1097-1124, called from
  the wider peak-rebuild routine ending at 1133). Pulls `p.getStems()`
  (processor's current `StemSet`), builds 6 per-pixel min/max peak arrays at
  the current zoom (`viewStart`/`viewSpan`), one per stem, same peak-decimation
  idiom as the composite `peaks` array above it.
- Processor state:
  - `padStemMask` — `Source/PluginProcessor.h:229-238` (mask accessors),
    backing storage `std::array<std::atomic<std::uint8_t>,16> padStemMask {}`
    at line 438 (spec cited "h:438" — exact). 0 = FULL (plays the master
    render); non-zero = bitmask of the up-to-6 stems this pad plays from.
    Written ONLY by `setPadStemBit`/`setPadFull` (lines 232-238), which are
    called from the inspector's SOURCE chip row — **not** from anything
    hero/lane-related.
  - `stemMuted`/`stemSoloed` — accessors at `Source/PluginProcessor.h:223-226`,
    backing storage `std::array<std::atomic<bool>,6>` at lines 436-437 (spec
    cited exact). Global (not per-pad) mute/solo, written only by the lane
    pill/S-box hit-tests above.
  - Three-spot persistence keys `smute*`/`ssolo*`:
    - `saveKit()` writes them at `Source/PluginProcessor.cpp:2129-2130`
      (spec cited exact) inside `saveKit` (defined at line 2108).
    - `getStateInformation()` writes them at `Source/PluginProcessor.cpp:2758-2759`
      (spec cited exact) inside `getStateInformation` (defined at line 2737).
    - Both **read back through the single shared `applyStateTree()`**
      (defined at line 2157), which restores them at lines 2192-2193 (spec
      cited exact) and is called from both `loadKit()` (line 2153) and
      `setStateInformation()` (line 2783). So there are two *write* spots
      (kit save, DAW/standalone session save) and one shared *read* spot —
      "three-spot" in the sense the project's other by-hand-persisted atomics
      (e.g. `edW`/`edH`, `stemQ`, `slG`/`slS`/`slP`, `snap`, `vel2l`) use:
      every one of them appears in both write functions and is read once in
      `applyStateTree`. `heroView` should follow this exact pattern (see §5).
  - `gent::stemGainFor` — `Source/EngineMath.h:265-274` (spec cited exact),
    a pure extraction of the per-voice per-stem gain table documented at its
    call site `Source/PluginProcessor.cpp:2382` (inside `processBlock`,
    spec's "~2422-2437" for the surrounding table has drifted — the current
    call site is line 2382, and the doc comment inside EngineMath.h itself
    says "~2422-2437", both pre-refactor artifacts; verified live at 2382).
    Semantics: FULL pad (mask 0) → every stem unity; on a filtered pad, the
    selected stem(s) stay unity and the rest "bleed" in at
    `clamp(bleedParam,0,1)*0.5`; `globalAudible=false` forces 0 regardless
    (global mute/solo always wins over pad source selection).
  - `globalAudible` derivation — `Source/PluginProcessor.cpp:2301-2313` (spec
    cited exact): computed once per `processBlock` from `stemMuted`/`stemSoloed`
    — solo-any wins (any soloed stem is audible, everything else silent),
    else mute-only (muted stems silent, rest audible).
  - Preview honoring mute/solo — `Source/PluginProcessor.cpp:2631-2699`ish
    (spec cited 2644-2686, confirmed inside range: the preview block starts
    at 2635, uses `globalAudible[k]` at 2644/2651/2670/2685 exactly as
    cited). Preview ramps its own `prevStemSg[6]` toward `globalAudible`
    targets over ~6ms — click-free like the voice path.
  - Async separation lifecycle:
    - `stemStatus` (`juce::String`, guarded by `infoLock`) — declared
      `Source/PluginProcessor.h:415`; narrates progress/errors, read via
      `getStemStatus()` (h:212, cpp:953-957).
    - `renderedStems`/`activeStems` (`RenderedStems::Ptr`) — declared
      `Source/PluginProcessor.h:430-431`; worker writes `renderedStems`
      (`doStemRenderJob`, cpp:1226+, assignment at 1276), audio thread swaps
      in `activeStems = renderedStems` at cpp:2234 (a message/audio handoff
      point, not lock-free-atomic — outside D's regression fence, untouched
      either way).
    - `stemRenderGen` — the spec's cited symbol name maps to
      `int stemRenderGeneration = 0;` (`Source/PluginProcessor.h:433`) plus
      per-`RenderedStems` `stemSetGeneration` set at `cpp:1241`; used to
      detect stale renders after a re-separation, not touched by D.
    - `separating`/`downloadingModels`/`stemProgress`/`stemSet` — booleans/
      float/`StemSet::Ptr` declared `PluginProcessor.h:401-408`;
      `hasStems()` (cpp:941-945) is `stemSet != nullptr` under `stemLock`.
      `requestStemSeparation()` (cpp:935-939) just sets `wantStems` and
      wakes the worker; `doStemJob()` (cpp:959+) does the real work
      (model download → init → separate → publish `stemSet`, line ~1099).
  - **Finding not flagged by the spec:** `loadFile()`
    (`Source/PluginProcessor.cpp:229-273`) does **not** clear `stemSet`,
    `stemMuted`/`stemSoloed`, or `padStemMask` when a new source is loaded.
    Loading a second file over an existing separated one leaves the OLD
    `StemSet` (and its mute/solo/source-mask state) live and displayable
    until the user re-separates or the worker overwrites it. This matters
    directly for D1 §6's matrix and is addressed as DECISION D1-DECISION-6
    below — the spec's D5 scope line ("New-file load resets stems
    availability without nuking the view request") assumes a reset that
    does not exist in code today.

## 2. Mockup ground truth

Source: `C:\Users\JoeyD\Downloads\gentsampler_redesign_v2.html`. All line
numbers below were read directly from that file at authorship time.

- **`#viewSeg` markup** — HTML lines 247-253:
  ```html
  <div class="ov tr">
    <div class="seg" id="viewSeg">
      <div class="s on" data-v="comp">Composite</div>
      <div class="s" data-v="stems">Stems</div>
    </div>
    <div class="chip caret">HQ</div>
  </div>
  ```
  Order in the top-right overlay is **seg first, then the HQ caret-chip**,
  both children of `.ov.tr`. Neither segment carries a `<small>` subtitle
  (the CSS at `.seg .s small` (line 96) supports one — used elsewhere, e.g.
  the Trigger seg at lines 286-288 — but `#viewSeg`'s two segments are
  plain single-line labels "Composite"/"Stems").
- **`.seg` / `.seg .s` CSS** — lines 90-102: segmented control, dark well
  background, `.s.on` gets the amber tint/glow treatment
  (`rgba(232,153,58,.28→.12)` gradient, `0 0 10px` glow, text `#FFD9A3`).
  This is the same primitive already used for the Trigger seg — Phase A/B
  chrome conventions (REDESIGN_TASK_D.md ground rule 6) apply unmodified.
- **`drawStems()`** — HTML lines 469-491. Full findings:
  - `laneH = H / 6` (line 472) where `H` is the canvas's backing height
    (320, for a 1040×160 hero element — see the 2× note below).
  - Alternating lane bed: odd-indexed lanes (`li % 2`, i.e. lanes 1,3,5 —
    BASS, GTR, OTHER) get a `rgba(255,255,255,.015)` fill (line 476); even
    lanes get no bed fill.
  - 1px separator `rgba(0,0,0,.45)` at the bottom of every lane
    (`y0 + laneH - 1`, height 1 — line 477), for all 6 lanes including the
    last (drawn at the canvas bottom edge).
  - Lane waveform: per-stem colour columns, `globalAlpha = .8` (lines
    480-484), starting at canvas x=64 (`for(let x=64;...)`) — i.e. the
    columns begin to the right of the label plate (see next point), not at
    x=0.
  - Label plate: `rgba(10,13,16,.85)` fill, `0..58` px wide (canvas space)
    × full lane height (line 486); stem-hued bold mono label text
    (`700 12px ui-monospace...`, line 487) at x=10, vertically centered
    (`mid+4`, line 488).
  - `drawFlags(g,W,H)` is called at the end (line 490) — full-height,
    identical function to composite's.
  - **No mute affordance, no solo affordance, no interactive element of any
    kind is drawn in `drawStems()`.** It is a pure, static, non-interactive
    paint function — confirmed by reading every line; there is no click
    target logic anywhere in this function or registered against the
    `#wave` canvas for lane clicks.
- **`drawFlags()`** — HTML lines 434-451, shared by both views (called from
  `drawComposite()` line 464 and `drawStems()` line 490): draws all 15 mock
  slice flags as numbered, stem-hued pennants (bold mono numeral, glow-blur
  9, 25×19px flag body) full-height (`g.moveTo(x,0);g.lineTo(x,H)` — spans
  the ENTIRE canvas height in both views, not just a `flagBarY` strip); then
  the active cue region as an amber-tinted fill + stroked rect
  (`rgba(232,153,58,.10)` fill, `rgba(255,184,92,.65)` 2px stroke,
  lines 444-445); then a glowing playhead (soft horizontal gradient +
  1.5px-either-side solid centre line, lines 446-450) — also full-height in
  both views.
- **`drawComposite()`** — HTML lines 453-467: full-height amber composite
  columns with the `.85/.28/.70` vertical gradient (matches the ported
  JUCE composite paint, `PluginEditor.h:438+`), `drawFlags()` full-height
  (line 464), and — **only in this view** — a bottom time-ruler
  (`rgba(155,161,168,.5)` @ 10px mono, 5 fixed labels, line 465-466). There
  is NO lanes band anywhere in `drawComposite()`.
- **Toggle wiring** — HTML lines 494-499:
  ```js
  document.getElementById('viewSeg').addEventListener('click',e=>{
    const s=e.target.closest('.s');if(!s)return;
    document.querySelectorAll('#viewSeg .s').forEach(x=>x.classList.remove('on'));
    s.classList.add('on');
    s.dataset.v==='stems'?drawStems():drawComposite();
  });
  ```
  Pure presentation: a click swaps the `.on` class and calls one of two
  paint functions. No model state, no audio hook, no persistence — the
  mockup is a static HTML file with no concept of a session.
- **Mutual exclusivity confirmed:** exactly one of `drawComposite()` /
  `drawStems()` ever runs; there is no partial/blended state, no third
  mode, and (per the mockup's own subtitle at HTML line 214) the mockup
  ships with Composite selected by default (`.s.on` on the `comp` segment
  at load, line 249).
- **2× canvas-backing note:** the `<canvas id="wave" width="2032" height="320">`
  (HTML line 242) backs a `.hero` element that is `height:160px` and — at
  the 1040px `.plugin` width (line 29) minus the hero's `margin:8px 12px`
  (CSS line 105) — effectively ~1016px wide on screen. That is a 2:1
  canvas-to-CSS-pixel ratio on both axes. All of `drawStems()`'s literal
  pixel values above (laneH divisor aside, which is ratio-based) — the 58px
  label-plate width, the x=64 column start, the 1px separator, the 25×19
  flag body — are **canvas-space** and read as roughly half those values in
  real hero pixels, consistent with how Phase C interpreted `drawFlags`/
  `drawComposite` literals (halved, then the side-by-side capture arbitrates
  the residual). D3 must follow the same halving-then-sbs-arbitrates method;
  this doc does not re-derive exact JUCE pixel values — that is D3's job,
  arbitrated by the mandatory sbs capture (REDESIGN_TASK_D.md ground rule 5).

## 3. Mode semantics

**COMPOSITE vs STEMS is a pure presentation switch on the hero. It changes
what the `WaveformView` paints and nothing else**, per REDESIGN_TASK_D.md
ground rule 2 (ratified default, adopted as-is — this doc found no code
reason to disagree):

- **Playback:** no effect. `processBlock` (the entire audio path named in
  the regression fence) does not read the view mode. What plays on a pad hit
  is determined exclusively by `padStemMask` via `gent::stemGainFor`
  (`Source/EngineMath.h:265-274`) and global `stemMuted`/`stemSoloed` via
  `globalAudible` (`Source/PluginProcessor.cpp:2301-2313`), exactly as today.
- **Per-pad source selection:** no effect. The inspector's SOURCE chip row
  (FULL/DRM/BASS/VOX/GTR/PNO/OTH, mockup HTML lines 293-304) remains the
  only writer of `padStemMask` (`setPadStemBit`/`setPadFull`,
  `PluginProcessor.h:232-238`). The hero view toggle does not gain a new
  `padStemMask`-writing affordance under this default (see §7 for the one
  place that tension resurfaces — mute/solo, which is a DIFFERENT piece of
  state from `padStemMask`).
- **Preview/audition:** no effect. Preview (`startPreview`/`stopPreview`,
  `PluginProcessor.h:259-260`, engine at `cpp:2631-2699`) is driven by
  `previewCmd`/`assignCursor`, neither of which the hero view touches.
- **Pad grid / inspector:** unaffected. Nothing in `PadButton`, the
  inspector's SOURCE row, Trigger seg, or grain controls reads the hero
  view mode.
- **Mid-playback lane switch:** repaint only. The toggle is read and
  written **on the message thread only** — it is UI state, not audio-thread
  state, so switching COMPOSITE↔STEMS while pads are sounding has zero
  audio-thread interaction; only the hero's `paint()` changes what it draws
  on the next frame. This mirrors how `stemBandTop`/`stemBandH`/`hoverLane`
  already work today (message-thread-only fields consumed by the message-
  thread `paint()`/`mouseDown`/`mouseMove`).

## 4. State model

- **Location:** `std::atomic<int> heroView` on `GentSamplerAudioProcessor`,
  declared alongside the existing editor-close-proof precedent
  `std::atomic<int> editorW { 900 }, editorH { 640 };`
  (`Source/PluginProcessor.h:174`). Same rationale that precedent
  established: editor-close-proof state that isn't an APVTS parameter
  belongs on the processor as a plain atomic, not on the editor (which is
  destroyed/recreated across editor open/close and DAW GUI detach/reattach).
- **Legal values:** `0 = COMPOSITE`, `1 = STEMS`. Two values only — the
  mockup defines no third mode (§2).
- **Who reads/writes:**
  - Written by: the new seg control's click handler (message thread, D2).
  - Read by: `WaveformView::paint` (to pick the paint branch, D3) and
    `WaveformView::mouseDown`/`mouseMove` (to pick the hit-test set, D4) —
    both message-thread-only, per §3's "read on the message thread only"
    rule. `processBlock` never reads it (regression fence).
- **Sanitization:** a pure `gent::sanitizeHeroView(int stored)` function
  (per REDESIGN_TASK_D.md D2 scope) clamps any out-of-range stored value
  (negative, `>1`, garbage from a hand-edited or corrupted kit file) to
  `0` (COMPOSITE) — the safe, always-valid default. This exists specifically
  for the persistence-restore path (§5) and is unit-tested per D2's
  ctest additions (negative/large/garbage ints).
- **Effective vs requested:** per §5/§6, the atomic stores the **request**
  (what the user last asked for); a separate pure function
  `gent::resolveHeroView(int requested, bool stemsAvailable)` computes the
  **effective** view actually painted. `heroView` itself is never silently
  overwritten by availability changes — only the effective-view computation
  changes. This is what makes the "sticky request" behavior in §5/§6
  possible without a second piece of state.

## 5. Persistence

**DECISION D1-DECISION-1 — persist heroView? Recommended: YES, three-spot,
key `"heroView"`.**

- **Gap:** the mockup has no concept of persistence (it's a static page).
  Whether the user's last COMPOSITE/STEMS choice should survive
  project save/reload, kit save/load, and editor close/reopen is undefined
  by the mockup and must be decided here.
- **Options:**
  1. Persist via the three-spot pattern (`saveKit`/`getStateInformation`
     write; `applyStateTree` reads), same as `edW`/`edH`/`stemQ`/`snap`/etc.
  2. Session-only (survives editor close/reopen within the same processor
     instance, since it's already a processor atomic — but does NOT survive
     project/kit save-reload).
  3. Not persisted at all — always resets to COMPOSITE on load.
- **Recommended default: Option 1 (three-spot).** Rationale: the atomic
  already lives on the processor (editor-close-proof for free, matching
  option 2's benefit at zero extra cost), and every other per-project UI
  preference the codebase has chosen to keep (`slG`/`slS`/`slP`, `snap`,
  `vel2l`, `edW`/`edH`) uses exactly this pattern — a returning user
  reopening a saved kit expects their STEMS-view choice to still be there,
  the same way they expect their slice-grid or snap toggle to be. The
  R2 gate the spec already schedules ("Three-spot persistence has a *silent*
  failure class... ctest can't see ValueTree plumbing") exists precisely to
  catch a missed spot, so the machine-unwatchable risk this introduces is
  explicitly gated, not unmitigated.
  Key: `"heroView"`, added to the `smute*/ssolo*` neighborhoods in
  `saveKit` (cpp:2129-2130 vicinity), `getStateInformation` (cpp:2758-2759
  vicinity), and read back once in `applyStateTree` (cpp:2192-2193
  vicinity) via `sanitizeHeroView`. Old projects/kits without the key
  default to `0` (COMPOSITE) — the same "default audible for older
  presets" fallback pattern already used for `smute*/ssolo*` (cpp:2190
  comment: "restore stem mute/solo (default audible for older presets)").
- **Joe: ACCEPTED — recommended default ratified (2026-07-03, relayed via session; "all recommendations are good").**

**Interaction with kit save/load:** both `saveKit`/`loadKit` (user-triggered
`.gentkit` files) and `getStateInformation`/`setStateInformation` (DAW
session / standalone last-state) carry the same `EXTRA` ValueTree today —
`heroView` rides in the same child node, no new ValueTree or file needed.

**The missing-source case (standalone silent-source landmine, CLAUDE.md
"Known landmines" 2026-07-02):** the standalone wrapper persists/restores
the last-loaded file itself (JUCE Standalone plumbing, not custom
GentSampler code — no `heroView`-adjacent symbol exists for it, confirmed
by grep: no match for "lastFile/recentFile/restoreLast" in
`PluginProcessor.cpp`). That restored source can come back silent/empty
(a WAV that fails to decode meaningful audio, or — per the landmine note —
a slice-export artifact like `GentSampler_Pad12.wav` reloading as an empty
buffer). If the persisted request is STEMS but the restored source has
`hasStems() == false` (no separation has run against it, or `loadFile`
left a stale `StemSet` from a PRIOR source per the §1 finding):

- **Default:** the request is sticky (the atomic keeps storing `1` =
  STEMS); the **effective** view resolves to COMPOSITE via
  `gent::resolveHeroView(1, false) → 0` until stems become available for
  the current source; the hero paints COMPOSITE (or the empty/no-source
  state, `PluginEditor.h:301-303`, silently showing composite-branch
  furniture that is itself empty when there's no waveform) in the
  meantime. The moment stems appear (fresh separation completes, or — see
  the §1 finding — a stale `StemSet` from the previous source happens to
  still be non-null), `resolveHeroView` flips the effective view to STEMS
  automatically, with no further user action, because the request never
  changed.

**DECISION D1-DECISION-2 — does the seg control display the REQUEST or the
EFFECTIVE view? Recommended: EFFECTIVE.**
- **Gap:** REDESIGN_TASK_D.md explicitly flags this as unresolved ("DECISION
  which of request/effective the seg shows").
- **Options:**
  1. Seg shows the sticky REQUEST — the STEMS segment stays lit even while
     the hero is actually painting COMPOSITE (because stems aren't ready
     yet), signaling "this is what you asked for, it'll switch when ready."
  2. Seg shows the EFFECTIVE view — the seg reflects what's actually
     painted right now; it can silently show COMPOSITE lit even though the
     stored request is still STEMS, then flip to STEMS-lit the instant
     stems become available.
- **Recommended default: Option 2 (effective).** Rationale: a seg control
  that shows itself "on" for a view that isn't actually on screen is a
  direct visual lie a user can act on (e.g. clicking Composite thinking
  they're switching FROM Stems, when Stems was never actually showing) —
  and the mockup's seg is a simple two-state radio with no third
  "pending"/disabled-but-selected visual language to borrow from (§2: no
  mute/solo, no third state, nothing conditional at all is drawn in the
  mockup's seg). Showing the effective view keeps the control honest at the
  cost of one surprising-but-rare moment (the seg silently "changes itself"
  back to Stems once stems arrive) — which is far less confusing than a
  control that lies about the current picture for however long separation
  takes (could be minutes, per `doStemJob`'s multi-minute MAX-quality path,
  `PluginProcessor.cpp:1047-1058`).
- **Joe: ACCEPTED — recommended default ratified (2026-07-03, relayed via session; "all recommendations are good").**

## 6. Async separation lifecycle × view — the full matrix

All seven rows below are read from `Source/PluginProcessor.cpp`'s
`doStemJob()` (959+) and its status booleans (`separating`,
`downloadingModels`, `stemProgress`, `hasStems()`), the same predicates the
editor already reads at `PluginEditor.cpp:1551-1571` for `stemStatusLbl`
(spec cited 1560-1570; the live block spans 1551-1571, confirmed).

**Reading the "resolves to placeholder" wording (R1 clarification):**
`resolveHeroView(requested, stemsAvailable)` is strictly binary — it returns
COMPOSITE or STEMS, never a third value. "Placeholder" is a CONTENT decision
made *inside* the STEMS paint branch when it renders with no stems, not a
visibility gate on the branch itself (§9 states this; it applies to this
whole table). Rows 1, 2, 3, 4, and 6 all share `stemsAvailable == false` at
the `resolveHeroView` layer; what differs between them is only what the
STEMS branch paints (placeholder flavor + status text) when the sticky
request selects it.

| # | Row | `hasStems()` | `separating` | `downloadingModels` | Seg (COMPOSITE / STEMS) | Hero paints |
|---|---|---|---|---|---|---|
| 1 | No source loaded | false | false | false | both enabled, COMPOSITE effective/shown regardless of request | empty-state chip prompt (`PluginEditor.h:260-299`, unchanged) |
| 2 | Source loaded, never separated | false | false | false | COMPOSITE enabled; STEMS enabled but resolves to placeholder (see DECISION-3) | COMPOSITE: full composite wave. STEMS (if requested): placeholder lanes (DECISION-3) |
| 3 | Downloading models (first run) | false | true | true | both enabled; STEMS shows placeholder+status | COMPOSITE: full composite wave (separation runs in background, doesn't block the view). STEMS (if requested): placeholder + `stemStatusLbl` text ("Downloading models NN%") |
| 4 | Separating N% | false | true | false | both enabled; STEMS shows placeholder+status | Same as row 3 but status reads "Separating NN%" |
| 5 | Stems ready | true | false | false | both enabled | COMPOSITE or STEMS per effective view — STEMS now paints real 6-lane data |
| 6 | Separation failed | false | false | false | both enabled; STEMS resolves to placeholder | COMPOSITE: unaffected. STEMS (if requested): placeholder + `stemStatusLbl`'s error text (`doStemJob`'s `setStatus("model download failed: ...")` / `"init failed: ..."` / separator-thrown-exception text, all surfaced through the same `getStemStatus()` the label already reads) |
| 7 | Stems present from restore (kit/session reload with `hasStems()==true` at load) | true | false | false | both enabled | Effective view resolves per the sticky-request rule (§5) — if request was STEMS, STEMS paints immediately with the restored `StemSet` |

**DECISION D1-DECISION-3 — pre-separation STEMS behavior (mockup is
silent). Recommended: seg enabled always; STEMS view shows the six lane
plates + a "SEPARATE STEMS to fill the map" placeholder wired to
`sepStemsBtn`.**
- **Gap:** the mockup ships with stems already "separated" (its STEMS view
  always has fake waveform data — there is no pre-separation state in a
  static HTML mockup at all). Rows 1-4 and 6 above have no mockup reference.
- **Options:**
  1. Disable the STEMS segment entirely until `hasStems()` is true (user
     literally cannot click into an empty view).
  2. Enable STEMS always; painting empty/placeholder lane plates (label
     only, no waveform data) with a call-to-action referencing
     `sepStemsBtn`, plus the live `stemStatusLbl` text during download/
     separation/failure.
  3. Enable STEMS always; auto-request separation on first click if none
     has run (silently starts the multi-minute job from a view click).
- **Recommended default: Option 2.** Rationale: Option 1 (disabling the
  segment) hides the destination entirely — a new user has no way to
  discover the Stem Map exists until after they've already found and
  clicked "Separate Stems" elsewhere, which works against discoverability
  for a headline v2 feature. Option 3 silently kicks off an expensive
  (multi-minute on CPU per `doStemJob`'s MAX-quality path) background job
  from what looks like a passive view toggle — surprising and against
  ground rule 2's "presentation only" spirit for the seg. Option 2 keeps
  the seg always reachable (matches the UI acceptance criterion "COMPOSITE
  ⇄ STEMS reachable in 1 click from the default view" for ALL lifecycle
  stages, REDESIGN_TASK_D.md's Verification section), shows the six real
  lane labels/colours so the destination is self-explanatory, and reuses
  the already-wired `sepStemsBtn`/`stemStatusLbl` machinery with zero new
  audio-adjacent code.
- **Joe: ACCEPTED — recommended default ratified (2026-07-03, relayed via session; "all recommendations are good").**

**No auto-view-flip on separation completion.** When separation finishes
mid-session (row 4 → row 5 transition), the view does **not** change by
itself unless the user's own sticky request was already STEMS (§5's
resolve rule handles that automatically — it is not a special-cased "flip
on completion" behavior, just the natural consequence of `stemsAvailable`
changing under a constant `requested`). If the request was COMPOSITE, it
stays COMPOSITE even after stems become ready — no surprise view flips
(REDESIGN_TASK_D.md §6 explicit requirement).

**DECISION D1-DECISION-6 — does loading a NEW file over an already-separated
one reset stems availability? Recommended: YES — `loadFile` should clear
`stemSet` (and reset `stemMuted`/`stemSoloed`/`padStemMask`... but see the
undo/regression-fence caveat below).**
- **Gap:** REDESIGN_TASK_D.md's D5 scope line states "New-file load resets
  stems availability without nuking the view request" as if this already
  happens. Per §1's finding, it does **not** — `loadFile()`
  (`PluginProcessor.cpp:229-273`) never touches `stemSet`, `stemMuted`,
  `stemSoloed`, or `padStemMask`. Today, loading file B after separating
  file A leaves file A's `StemSet` fully live, playable, and paintable
  against file B's (unrelated) waveform/cues until the user re-separates.
  This is an existing bug-shaped gap, not something D introduces — but D's
  STEMS view is what will make it suddenly VISIBLE (today's always-on band
  already technically has this bug too, since `rebuildStemPeaks()` just
  calls `p.getStems()` unconditionally, but a thin 78px band under the
  composite wave is far less noticeable than a full-hero 6-lane view
  showing yesterday's song's stems under today's song's flags).
- **Options:**
  1. Leave `loadFile` untouched (out of scope — D only adds view state,
     per the regression fence). The stale-`StemSet`-survives-a-new-load
     behavior is pre-existing and orthogonal to D's job.
  2. Have `loadFile` clear `stemSet` (`stemLock`-guarded, set to `nullptr`)
     so `hasStems()` correctly reports false for the new source, restoring
     row-1/row-2 matrix behavior on every file load. Leave `stemMuted`/
     `stemSoloed`/`padStemMask` untouched (they're independent settings a
     user might reasonably want to carry forward, e.g. "always solo
     vocals").
  3. Same as 2, but also reset `stemMuted`/`stemSoloed`/`padStemMask` to
     defaults on new-file load (full clean slate).
- **R1 clarification (restore path already correct):** the `applyStateTree`
  restore path is NOT part of this gap — `PluginProcessor.cpp:2170` calls
  `loadFile(f, false)` and then unconditionally restores `padStemMask` from
  the `"src"+i` keys at line 2177, so project/kit restore already lands in a
  correct mask state today. The Option-2 clear therefore targets only the
  two direct user-load call sites (`PluginEditor.cpp:311` drag-drop,
  `PluginEditor.cpp:1261` file browser); the restore path needs no change.
- **Recommended default: Option 2**, flagged as an **explicit scope
  exception requiring Joe's authorization** — `loadFile` is
  `PluginProcessor.cpp`, squarely inside files D's own regression fence
  does NOT exclude (the fence excludes `processBlock`/the audio path/
  `padStemMask` semantics/`SOURCE` chips/`smute*/ssolo*` keys, but says
  nothing about `loadFile`'s stem-clearing behavior — this is a genuine
  gap in the fence, not a violation of it). Rationale for Option 2 over 1:
  D5's own scope line already assumes this reset exists, and D5's kill-case
  acceptance criterion ("persisted STEMS + missing source on standalone
  relaunch → composite/empty renders, STEMS engages by itself once stems
  return") is only fully meaningful if "stems return" means *newly
  separated for the current file*, not *stale stems from whatever file was
  loaded before the restore*. Rationale for 2 over 3: clearing
  `stemMuted`/`stemSoloed`/`padStemMask` on every file load is a bigger
  behavior change with its own UX cost (losing a carefully-tuned mute/solo
  setup because you loaded a new file) and isn't needed to make the matrix
  correct — `hasStems()==false` alone is sufficient to gate the STEMS view
  back to placeholder/composite. If Joe wants Option 1 instead (truly
  zero-touch, defer this to a future bugfix separate from D), D5's own
  scope line and kill-case wording need a one-line correction to stop
  implying a reset that isn't happening.
- **Joe: ACCEPTED — recommended default ratified (2026-07-03, relayed via session; "all recommendations are good").**

## 7. Mute/solo relocation (the big silent gap)

**Today:** mute (label pill, click) and solo (hover "S" box, click) live
inside the always-on lanes band — which, since there is no toggle today,
is effectively shown whenever stems exist, i.e. in what will become the
COMPOSITE-adjacent default view once D ships (D's COMPOSITE view has no
band at all, per D3's scope: "COMPOSITE branch: delete the lanes band
entirely").

**The mockup's composite view has no lanes; its STEMS view draws no
mute/solo affordance anywhere** (§2 — confirmed by reading every line of
`drawStems()`; there is no click-target logic of any kind registered
against `#wave`). Shipped, persisted functionality (mute/solo + its
`smute*/ssolo*` three-spot persistence, §1) may not be lost per
REDESIGN_TASK_D.md's explicit "Full stop" requirement.

**DECISION D1-DECISION-4 — where do mute/solo live post-D? Recommended:
carry current semantics unmodified into the full-height STEMS lanes (label
plate click = mute, hover-S-box-at-lane-right = solo), and accept that
mute/solo become reachable ONLY from STEMS view.**
- **Gap:** the mockup is silent on where these controls go; today's home
  (the band) is being deleted.
- **Options:**
  1. Move mute/solo into the STEMS-view lanes (each of the 6 lanes gets a
     mute-click zone at its label plate and a hover-solo zone at its right
     edge, geometrically identical logic to today's band, just re-hosted
     at full lane height instead of 13px/lane).
  2. Keep a small always-visible mute/solo affordance in the COMPOSITE view
     too (e.g. a compact 6-swatch row somewhere in the hero chrome) so
     users don't have to switch views to mute a stem.
  3. Move mute/solo out of the hero entirely (e.g. into the inspector, or a
     new small panel) so it's reachable from both views without adding
     chrome the mockup doesn't show in either.
- **Recommended default: Option 1.** Rationale: it's the option with zero
  new visual design to invent — REDESIGN_TASK_D.md ground rule 3 (chrome
  conventions) and the "mockup is the spec" framing (line 4) both push
  toward reusing exactly what the mockup already draws (six full-height
  lanes with label plates) rather than inventing new chrome the mockup
  never shows (Option 2) or relocating shipped functionality to an
  unspecified new home with its own layout risk (Option 3). It also keeps
  D's blast radius to "port existing hit-test logic to new geometry" (D4's
  own framing) rather than "design a new control."
- **Joe: ACCEPTED — recommended default ratified (2026-07-03, relayed via session; "all recommendations are good").**

**DECISION D1-DECISION-5 — is mute/solo-reachable-only-in-STEMS-view an
acceptable UX regression for composite-dwellers? Recommended: YES, with a
narrated escape hatch.**
- **Gap:** REDESIGN_TASK_D.md explicitly asks this as an open question, not
  a decision it pre-answers. A user working primarily in COMPOSITE view
  (the default, per §2's mockup load state) currently has one-click
  mute/solo always visible; post-D, per DECISION-4/Option-1, they'd need to
  flip to STEMS first.
- **Options:**
  1. Accept the regression: mute/solo requires switching to STEMS view
     first. One extra click, discoverable via the seg control that's
     always visible in `.ov.tr`.
  2. Reject the regression: require DECISION-4 to pick Option 2 or 3 above
     instead, so mute/solo stays reachable from COMPOSITE.
- **Recommended default: Option 1 (accept it).** Rationale: this is the
  direct, unavoidable consequence of "the mockup is the spec" (ground rule)
  combined with the mockup's composite view genuinely having zero lanes
  chrome — inventing a COMPOSITE-view mute/solo affordance the mockup
  doesn't show would itself be a mockup deviation, trading one gap for
  another. The regression is also mild in practice: mute/solo is a mixing/
  audition operation naturally paired with looking at the per-stem
  waveforms (which is exactly what STEMS view is for), so the one-click
  cost of switching views to do stem mixing is arguably the more coherent
  workflow, not a strictly worse one. This is explicitly a REGRESSION,
  though, and REDESIGN_TASK_D.md's landmine flag 4 requires it be
  "surfaced as D1 DECISION-§7, not slipped in" — it is surfaced here for
  Joe's explicit ruling, not silently accepted by the doc author.
- **Joe: ACCEPTED — recommended default ratified (2026-07-03, relayed via session; "all recommendations are good").**

**Tripwire (already flagged by the spec, restated for completeness):** if
Joe's ruling on DECISION-4/5 ever has a lane affordance WRITE to
`padStemMask` (as opposed to the global `stemMuted`/`stemSoloed` it writes
today), that decision must simultaneously resolve its own undo story (see
§9) or be rejected at the Joe gate — `padStemMask` writes are currently
undo-EXEMPT (BACKLOG extend-undo item), and D must not silently expand
what's undo-exempt.

## 8. Interaction map for STEMS view

**Recommendation: ALL composite interactions remain live in STEMS view.**
Per REDESIGN_TASK_D.md D1 §8's own recommendation, adopted as-is — this
doc found no reason to disagree:

- Flag click-select (`PluginEditor.h:763-779`) — live in STEMS, full-height
  per `drawFlags`'s full-height paint (§2).
- Cue/end handle drags — live in STEMS, same drag engine
  (`hitStartEdge`/`hitEndHandle`, `PluginEditor.h:969+`), same `pushUndo`/
  `CueSnap` path (§9).
- Scrub, wheel zoom (`PluginEditor.h:927-933`), shift/middle pan
  (line 752), scrollbar (line 742) — all live in STEMS, unchanged.

**Exact hit-test priority (replaces today's whole-band early return):**

Today, `mouseDown` tests the ENTIRE band rectangle FIRST (before scrollbar,
pan, flag-select, and edge/handle tests) and `return`s unconditionally for
any y inside the band, regardless of x (`PluginEditor.h:730-740`). This
must change so that in STEMS view:

1. **Lane plate (mute) zone** and **lane solo-box zone** claim clicks ONLY
   within their specific x-ranges (`x ≤ 4 + labW + 6` for mute,
   `x ≥ w - soloW - 6` for solo — the exact constants already used at
   lines 735/733) **at that lane's y-range** (`y` inside the lane computed
   from the STEMS-view full-height lane geometry, D3's `laneH = h_wave/6`).
2. **Everything else** — including clicks inside a lane's y-range but
   OUTSIDE its mute/solo x-zones (i.e., the middle of a lane, where the
   per-stem waveform column paints) — **falls through to the shared paths
   unchanged**: scrollbar (742-750), middle-pan (752-758), flag-select
   (763-780), then start/end handle hits (785+). This is the direct fix for
   the "whole-band early return... dies" landmine (REDESIGN_TASK_D.md
   landmine flag 3) — today's `return` at line 739 for ANY in-band y must
   become a narrower `return` that only fires for the two real hit zones.
3. **Hover-solo reveal** (`hoverLane`, `mouseMove` lines 941-944) — rewired
   to the same full-lane geometry as (1), so the S-box still only appears
   on hover over its own lane, now spanning the lane's true (much taller)
   height instead of 13px.
4. **In COMPOSITE view**, none of the lane hit-tests exist at all — there
   is no band, so no dead zones (D3 deletes the band paint; D4 must ensure
   no leftover geometry/hit-test fires when `heroView == COMPOSITE`).

**Undo/slices/grain, stated explicitly:** a slice edit (cue/end handle
drag) made while the hero is in STEMS view is the exact same `pushUndo()` /
`CueSnap` mutation as one made in COMPOSITE view — same function, same
undo stack, same snap engine, same `applySlices`. There is no second edit
path, no duplicated hit constants (REDESIGN_TASK_D.md ground rule 3,
adopted verbatim). The grain marker (Slice Detail strip's freeze-position
affordance, `grainOnFor`/`grainFreezeFor`/`getGrainPosFor`,
`PluginProcessor.h:244-251`) stays exactly where it is today — a
`SliceDetailStrip`-only affordance below the hero, entirely untouched by
D (the mockup's lanes show no grain marker of any kind, matching this).

## 9. Undo scope statement, geometry retirement, and the 160px fit answer

**Undo scope statement (D neither depends on nor advances the BACKLOG
undo item):**

Undo today (`pushUndo`/`undo`/`redo`, `PluginProcessor.cpp:326+`,
`CueSnap` struct at `PluginProcessor.h:380`) snapshots ONLY the 16 pads'
`cue[]`/`end[]` windows. It is already, and remains, silent on: the hero
view mode, mute/solo (`stemMuted`/`stemSoloed`), `padStemMask`, and grain
params — all of these sync correctly to whatever undo restores (they're
simply not part of what undo restores, per the existing CLAUDE.md landmine
"Undo scope is PARTIAL", 2026-07-02). **D adds only new view-mode state
(`heroView`), which is presentation-only by §3 and therefore not
undo-worthy by the same convention that already excludes mute/solo and
`padStemMask`.** Per REDESIGN_TASK_D.md's own framing: **D neither depends
on, nor advances, the BACKLOG extend-undo item.** The one tripwire (stated
in §7): if any Joe ruling on DECISION-4/5 introduces a NEW affordance that
WRITES `padStemMask` (as opposed to reusing the existing global mute/solo
atomics), that specific decision must carry its own undo resolution or be
rejected at the Joe gate — this doc's recommended defaults (§7) do NOT
trigger that tripwire, since they reuse `stemMuted`/`stemSoloed` exactly
as today, not `padStemMask`.

**Geometry retirement list** — the following become dead/repurposed once
D3/D4 land, and must not be left as orphaned logic (R3's job to verify
per REDESIGN_TASK_D.md's reviewer placement map):

- `bandH`'s `jlimit(78, 250, ...)` floor/ceiling computation
  (`PluginEditor.h:323-324`) — retired. In COMPOSITE view there is no band
  (`bandH` conceptually becomes `0` always). In STEMS view, lane height is
  instead the full hero's wave-area height divided by 6
  (`laneH = h_wave / 6`, per D3's scope and §2's mockup formula), not a
  clamped fraction of `h`.
- The `h > 60` clause inside `stemsShowing = showStems && h > 60`
  (`PluginEditor.h:310`) and `showStems` itself (lines 302-303) — retired
  as a stems-BAND visibility gate. STEMS view is no longer gated by "is
  there any peak data" for its OWN visibility (the seg makes it explicitly
  reachable per DECISION-3's "seg enabled always"), though the underlying
  showStems-style check may still matter for deciding whether to paint a
  placeholder vs. real lane data (that's a content question inside the
  STEMS branch, not a visibility gate on the branch itself).
- `stemBandTop`/`stemBandH` members (`PluginEditor.h:1136`) — repurposed:
  they currently mean "the always-on band's y-range for hit-testing." Post-D
  they should mean (or be replaced by) "the STEMS view's full lane-area
  y-range" — i.e. the same fields, same purpose (feeding the lane
  hit-tests), just fed by D3's new full-height geometry instead of the
  retired `bandH` formula. D3/D4 decide whether to rename or reuse; this
  doc does not mandate a specific field name, only that the *concept*
  (band-top/band-height feeding lane math) survives in some form since §8's
  hit-tests still need it.
- The `rulerH = 0` / no-top-ruler logic (`PluginEditor.h:311-316`) — stays
  retired (it already is); STEMS view continues to have no top ruler,
  matching the mockup (§2). Not a new retirement, just confirming D doesn't
  resurrect it.
- The whole-band early-return `return;` at `PluginEditor.h:739` — retired,
  replaced by D4's narrower per-zone returns (§8).

**The 160px fit answer:** yes — **the mockup's lane design fits the 160px
hero without new geometry.** At 1040×700 (hero wave-area height ≈ the full
160px hero, minus whatever chrome insets already apply — no top ruler, no
bottom ruler in STEMS per §2), 6 lanes divide evenly to roughly 26.7px
each; at the 880×592 aspect-locked floor (`PluginEditor.cpp:677`,
`setResizeLimits(880, 592, 1560, 1050)`), lanes come out to roughly 22.5px
each. Both figures comfortably beat today's 13px band-lanes (78px / 6).
**In STEMS view, lanes REPLACE the wave** (full-height, not stacked above
a shrunken wave the way today's band sits above a squeezed composite) —
no new geometry budget is required; the STEMS view simply claims the
entire hero the composite wave currently occupies alone.

---

## Addendum — Joe ruling 2026-07-03 (DECISION-2/3 interplay, post-ratification)

Raised during D2c implementation: does requesting STEMS with no stems paint
the placeholder, or does the effective-view rule suppress the STEMS branch
entirely (composite paints, placeholder never visible)?

**Joe's ruling: requesting STEMS with no stems SHOWS the placeholder with the
SEPARATE STEMS call-to-action.** Consequences, now normative:

- The hero's paint-branch selector is the **sanitized REQUEST**
  (`gent::sanitizeHeroView(heroView)`), not `resolveHeroView`. Request=STEMS
  always enters the STEMS branch.
- Inside the STEMS branch, `gent::resolveHeroView(request, stemsAvailable)`
  answers the CONTENT question exactly per the R1 §6 clarification: 1 → real
  lanes; 0 → the DECISION-3 placeholder (six lane plates + stem labels +
  "SEPARATE STEMS" CTA wired to `sepStemsBtn`). The D2a/D2b test contract
  for both functions is unchanged and stays green.
- The seg's active pill shows the painted view — which now always equals the
  request. DECISION-2's request-vs-effective distinction dissolves: with a
  placeholder always available, the STEMS branch is never suppressed, so
  displayed == requested == painted. §6's matrix rows read accordingly
  ("Hero paints" column governs; the "COMPOSITE effective/shown regardless
  of request" phrase in row 1 is superseded by this addendum for rows where
  a source is loaded; row 1 (no source at all) keeps the existing empty-state
  chip prompt in both requests).

## Addendum 2 — Joe rulings at the D6 gate (2026-07-03)

- **FL validation: PASSED** ("fl looking solid") — Phase D accepted.
- **stemStatus staleness (D5-flagged, R3-confirmed): FIX AUTHORIZED** — the
  narration string now clears on a genuinely new direct load, in a second
  `runAnalysis`-gated block inside `loadFile` (same DECISION-6 boundary,
  `infoLock` per `setStatus`'s own discipline). Restore path unaffected.

## Decision index (for Joe's convenience)

| # | Topic | Recommended default |
|---|---|---|
| D1-DECISION-1 | Persist `heroView`? | YES — three-spot, key `"heroView"` |
| D1-DECISION-2 | Seg shows request or effective view? | EFFECTIVE |
| D1-DECISION-3 | Pre-separation STEMS behavior | Seg enabled always; placeholder lanes + `sepStemsBtn` CTA |
| D1-DECISION-4 | Where do mute/solo live post-D? | Full-height STEMS lanes, same semantics (label=mute, hover-S-box=solo) |
| D1-DECISION-5 | Mute/solo reachable only in STEMS — acceptable? | YES, accepted regression (one extra click) |
| D1-DECISION-6 | Does new-file load reset stems availability? | YES — `loadFile` should clear `stemSet` (scope-exception, needs explicit authorization since it touches `PluginProcessor.cpp` beyond view-state) |

Joe's ruling on each (accept as recommended, or override) must be recorded
in the Sign-off record table at the top of this document before D2 begins.
