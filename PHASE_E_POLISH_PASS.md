# PHASE E — POLISH PASS (UI/UX Audit Remediation)

**Project:** GentSampler (JUCE / VST3, C++, CMake)
**Build command:** `cmake --build build --config Release --parallel` → VST3 artifact
**Context:** Full UI/UX audit completed against a live FL Studio screenshot (2026-07-06). Architecture and layout phases A–D are settled. This phase is polish only — no new features, no layout re-architecture beyond what is specified here. Visual language is locked: 1040×700 canvas (aspect-locked, floor 880×592), charcoal background, **single amber/gold accent**, arc-indicator knobs (270° sweep, glowR 0.18, glowA 0.5, arcW 0.09, hue 32°), three control primitives (chips, segmented selectors, arc knobs), grain/vignette overlay.

**Working discipline (non-negotiable):**
1. **Audit before planning.** Before touching any task, inventory the actual current code for the components involved. Do not assume state from this document — verify it.
2. **Mockup gate.** Any task marked `[MOCKUP GATE]` requires a standalone HTML mockup rendered and approved by Joe BEFORE any JUCE implementation. Stop and wait for sign-off.
3. **One task at a time.** Complete, verify, and get sign-off on each task before starting the next.
4. **Full clean files.** When delivering changed source files, deliver complete files, not diffs or partials.
5. **Side-by-side verification.** Each task ends with a before/after screenshot comparison against the acceptance criteria below.
6. **Floor check.** Every task must be verified at both 1040×700 and the 880×592 floor. No clipped or truncated text at either size.

---

## TASK E1 — Header Restructure `[MOCKUP GATE]` — P0

The header currently crams branding, file ops, musical context, host sync, input modes, and the active-pad chip into one 36px row. It fails visibly in four places and is the roughest region at the resize floor.

### E1.0 Audit step
Locate and inventory the header component(s): every child control, its label source, fixed widths, font sizes, and layout code. Report the inventory before proposing the mockup.

### E1.1 Remove the PAD chip
- **Delete the `PAD 14` chip from the header entirely.** The active pad is already displayed in the pad inspector panel (large pad number + status line). The header chip is redundant. Do not relocate it — remove it.
- Reclaim the freed width for the INPUT zone (see E1.2).

### E1.2 Four-zone layout
Restructure the header into four labeled zones, left to right, separated by 1px hairline dividers at low opacity (~12% bone):

| Zone | Contents |
|---|---|
| **FILE** | Wordmark + tagline, then LOAD · KIT · EXPORT chips |
| **MUSICAL** | SRC BPM (`/2  159.2  X2`), KEY dropdown, PITCH value chip |
| **SYNC** | Merged host cluster (see E1.4) |
| **INPUT** | Velocity/Keyboard mode selector, MIDI cluster (see E1.5) |

- All zone labels use the existing micro-caps label style, top-aligned; all values baseline-aligned on a single shared baseline.
- **One control height** for every chip/dropdown in the header. Pick the current majority height and enforce it.
- Wordmark stays; reduce the "STEM FLIP WORKSTATION" tagline opacity one step so it doesn't compete.

### E1.3 PITCH knob → value chip
- Replace the header PITCH arc knob with a **drag-value chip** styled like the BPM readout: label `PITCH` above, value `-5 st` inline.
- Behavior: vertical drag to adjust, double-click to type, right-click/ctrl-click reset to `0 st`. Same parameter attachment as the current knob — this is a control-style swap only, no parameter changes.
- This eliminates the current bug where the knob's `-5` readout clips below the header into the hero strip.

### E1.4 Merge HOST + TEMPO into one SYNC cluster
- Current state: `HOST 130.0  x0.82` plus a separate `TEMPO SY…` dropdown (label truncates with an ellipsis — ship-blocker) plus two unlabeled circular micro-icons.
- New cluster: `HOST 130.0 · SYNC ▾` where SYNC is the tempo-mode dropdown, **wide enough to render its longest option with zero truncation.** Audit the dropdown's option list and size to the longest string.
- The `x0.82` ratio becomes secondary text (smaller, ~50% opacity) after the host BPM, with a tooltip: "Playback ratio vs. source BPM."
- The two unlabeled circle icons: identify what they do during the audit step. Either give them recognizable glyphs + tooltips, or relocate/remove them. Report findings before deciding.

### E1.5 Consolidate REC MIDI + MIDI
- Two adjacent controls both say "MIDI." Replace with one **MIDI cluster**: a passive activity LED (lights on MIDI input, non-interactive) + a single record-arm toggle chip labeled `REC`.
- LED and toggle must be visually distinct (LED = small round indicator, no chip chrome).

### E1.6 Mockup gate
- Deliver a standalone HTML mockup of the full header at 1040px and 880px widths, pixel-styled to the existing skin (charcoal, amber, micro-caps, chip radii).
- **STOP. Wait for Joe's side-by-side sign-off before writing any JUCE code.**

### E1 Acceptance criteria
- [ ] No PAD chip in header
- [ ] No truncated/ellipsized static labels at 1040 or 880 width
- [ ] No text clipping outside the header bounds (PITCH readout fixed)
- [ ] Exactly one MIDI-labeled control group
- [ ] Single shared baseline for all header values; single chip height
- [ ] Four hairline-divided zones as specified

---

## TASK E2 — Numeric Formatting System — P0

Value readouts are currently inconsistent: `1` (no unit), `80` (no unit), `20000 Hz`, `0.00`, `0.10`, `1.00x`, `0 st`.

### E2.1 Implement a single `formatValue` utility used by every readout
| Parameter type | Format | Examples |
|---|---|---|
| Time (attack/release) | ms below 1000, then s, 1 decimal | `1 ms`, `80 ms`, `1.2 s` |
| Frequency | Hz below 1000, then kHz, 1 decimal | `250 Hz`, `20.0 kHz` |
| Semitones | signed integer + `st` | `0 st`, `-5 st`, `+7 st` |
| Ratio/speed | 2 decimals + `×` | `1.00×`, `0.82×` |
| Normalized (crush, reso, bleed, level) | 2 decimals | `0.00`, `0.10` |
| Pan | `L##` / `C` / `R##` | `L25`, `C`, `R50` |
| BPM | 1 decimal | `159.2`, `130.0` |

### E2.2 Tabular figures
- All numeric readouts use tabular (fixed-width) figures so values don't jitter horizontally while dragging. If the current font lacks tabular figures, use a monospaced numeric variant for value text only.

### E2 Acceptance criteria
- [ ] Every knob/chip readout in the plugin routes through `formatValue`
- [ ] ATTACK/RELEASE show units; CUTOFF shows kHz above 1 kHz
- [ ] No value jitter while dragging any knob

---

## TASK E3 — Color Discipline: Purple Removal + Amber Hierarchy — P0

### E3.1 Kill the purple
- The active pad's waveform thumbnail and corner indicator dot render magenta/purple — off-palette. Audit where this color originates (leftover skin constant vs. intentional state color).
- Replace: pad waveform thumbnails render in the same amber as the hero waveform (or desaturated bone at reduced opacity); active-pad ring = amber; playing state = brighter amber pulse on the ring.
- Grep the codebase for any remaining magenta/purple color constants and report them before removal.

### E3.2 Amber hierarchy
Amber currently marks too many simultaneous elements (SEPARATE STEMS, FOLLOW, COMPOSITE, GATE, STOP AT CUE, QUANTIZE, slice handles, HQ region). Enforce:
- **Solid amber fill:** primary action (SEPARATE STEMS) and the single active option inside each segmented selector — nothing else.
- **Amber text/outline only:** active-but-secondary toggles (FOLLOW, STOP AT CUE, etc.).
- **Bone text on charcoal:** everything inactive.
- Slice/window handles stay amber (they are the signature interaction element).
- Deliver a before/after full-window screenshot pair; target roughly half the current count of amber-highlighted elements at rest.

---

## TASK E4 — Hero Strip Cleanup — P0

### E4.1 Status text
- Replace `done (62.6 s, CPU)` with a `STEMS READY` badge (chip style, bone text) that fades out ~3 s after separation completes. Timing/backend details move to the badge's tooltip. During separation, keep the existing progress affordance.

### E4.2 Floating cue tag
- The `CUE 14 OPEN` tag currently clips at the strip's top edge and overlaps the waveform. It is redundant with the slice-detail strip. **Remove it.** If any anchored playhead/cue tag remains desirable later, it becomes its own proposal — not part of this task.

### E4.3 Filename display
- Middle-ellipsize long names (`Apocalypse-All The Peo…-Abm-[Master]`) instead of tail-truncating mid-token. Give the filename a fixed, left-aligned slot in the strip's top row; full name on hover tooltip.

### E4.4 Timeline ruler
- Anchor the first time label at `0:00` on the left edge; distribute remaining labels from there.

---

## TASK E5 — Chip Taxonomy + Inspector Consistency — P1

### E5.1 Control-type affordances
- **Toggles** (LOOP, REVERSE, STOP AT CUE, FOLLOW, SNAP…): show fill/outline state per E3.2. STOP AT CUE restyled to match LOOP/REVERSE chip geometry.
- **Momentary actions** (CLEAR, PREVIEW): plain chip, press feedback only, never a persistent lit state.
- **Page/view openers** (GRAIN, TRANSCRIBE if it opens a view): trailing `▸` glyph. Drop the redundant `GRANULAR` caption — chip becomes `GRAIN ▸`.
- Audit QUANTIZE: determine whether it is a toggle or an action, then style accordingly. Report before changing.

### E5.2 SOURCE row
- Unify abbreviation scheme: keep `FULL` as the composite chip, **visually separated** (small gap or hairline) from the six stem chips; stem chips all 3-letter: `DRM · BAS · VOX · GTR · PNO · OTH`. Tooltips with full names on all chips.
- **BLEED becomes contextual:** hidden (or disabled at ~30% opacity — Joe's call at mockup review, default: hidden) when SOURCE = FULL; when a stem is selected it appears docked at the end of the stem chip row, baseline-aligned with the chips.

### E5.3 SHAPE · FILTER split
- Split the single `SHAPE · FILTER` heading into two labeled sections: `AMP` (ATTACK, RELEASE, CRUSH) and `FILTER` (CUTOFF, RESO, TYPE), replacing the anonymous vertical divider.
- Move the CHOKE dropdown out of this row — it is pad-routing behavior. Relocate it adjacent to the TRIGGER selector in the inspector's top row. Verify space at the 880 floor.

### E5.4 TRIGGER sublabels
- The per-segment sublabels (`hold`, `tap-fire`, `tap on/off`) are too small to read. Remove them from inside the segments; add a single caption line under the segment group that updates with the selection: `Hold to play` / `Tap to fire` / `Tap on / tap off`.

### E5.5 Slice-detail right column
- Fixed ~90px gutter for the CUE/END/LEN column so labels never crowd the waveform edge.
- When END is open, LEN displays `—` (not a third `OPEN`).
- Fix the small `OPEN` tag colliding with the handle graphic (offset or suppress when the handle is at the edge).

---

## TASK E6 — Micro-polish Sweep — P2

- Unify corner radii and control heights across header, hero strip, and inspector (currently ~3 chip heights).
- Enforce the 8px spacing grid; reconcile pad-grid gutter vs. inspector panel padding.
- Tooltips on every abbreviation (OTH, RESO, PNO, HQ, etc.).
- Hover affordance on empty pads (`+` brightens) and a distinct drop-target highlight state.
- Final floor check: full pass at 880×592 — zero clipped or truncated text anywhere.

---

## Execution order and gates

```
E1 (audit → mockup → SIGN-OFF → implement → verify)
→ E2 → E3 → E4        (implement → screenshot verify each)
→ E5 (E5.2/E5.3 need a quick mockup if layout shifts — Joe's call)
→ E6 → final side-by-side vs. audit screenshot
```

Do not batch tasks. Do not proceed past a gate without explicit approval. At each session boundary, write a handoff note recording task status against the acceptance checklists above.
