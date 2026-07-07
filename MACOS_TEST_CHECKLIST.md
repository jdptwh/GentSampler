# GentSampler macOS Test — Logic Pro (AU) Checklist

**PRIVATE TESTING ONLY.** This document is for one specific friend-and-Zoom
test session (MACOS_PORT_SPEC.md, Amendments A1+A2) and is never intended to
become public install guidance — the bundle it tests is ad-hoc signed, not
notarized, and the quarantine-strip step below is a private-testing-only
workaround, not a supported install path. Joe is the sole PASS arbiter; the
friend is the hands, not the judge (loop-4 human touchpoint).

You need exactly three things: **the .component (primary)**, optionally the
**.vst3** (bonus row only), and **Logic Pro**. No git, no Xcode, no build
tools — that is the point of this test. Joe sends the artifact (downloaded
from the CI run, ditto-zipped) by AirDrop/file transfer; you never get repo
access or source.

## 0. Pre-flight

Open **Terminal** and run:
```
uname -m
sw_vers -productVersion
df -h /
```
Report back:
- `uname -m` output (e.g. `arm64` or `x86_64` — tells us which chip you have;
  architecture was unconfirmed at spec time, see Risk R13).
- macOS version.
- Free disk space on `/` — need **at least 5 GB free** (the separation model
  download alone is ~1.79 GB).
- **Logic Pro version** — open Logic Pro → Logic Pro menu → About Logic Pro.
  **Must be Logic Pro, not GarageBand.** GarageBand hosts AU plugins too, but
  it has no multi-output instrument support, which would silently shrink the
  multi-out row below to nothing — the test needs real Logic Pro.
  Any Mac able to run a current version of Logic Pro already satisfies our
  11.0 minimum macOS target automatically — no separate check needed.

## 1. Install

1. Copy `GentSampler.component` to:
   ```
   ~/Library/Audio/Plug-Ins/Components/
   ```
   (Finder: Go → Go to Folder… → paste that path. Create the `Components`
   folder if it doesn't exist.)
2. **(Optional, bonus row 9 only)** Copy `GentSampler.vst3` to
   `~/Library/Audio/Plug-Ins/VST3/` — only needed if you're also doing the
   optional FL Studio mac-trial smoke row.

## 2. MANDATORY: strip the quarantine flag

Every file that arrives on your Mac via AirDrop/download/file-transfer gets
an invisible "quarantine" tag from macOS, and Gatekeeper will refuse to run
an unsigned/ad-hoc-signed plugin that still carries it. Run this in Terminal
— it's safe, it only removes that one tag from this one plugin, nothing else
on your machine is touched:
```
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/GentSampler.component
```
Skip this and Logic will likely fail to load the plugin at all, or macOS
will show a "can't be opened" / damaged-file dialog that has nothing to do
with an actual bug — do this step first if you hit that.

## 3. Logic AU rescan

1. Quit Logic Pro if it's running, then relaunch it. Logic scans for new AU
   components at launch.
2. If GentSampler does NOT show up in the instrument list (step 4 below):
   open **Logic Pro → Settings → Plug-in Manager**, select GentSampler
   (or "AU Instruments" generally), and run a **Full Rescan**.
3. Logic re-runs Apple's own `auval` validator during this scan — since our
   CI already ran `auval -v aumu Gsmp Jtgt` and it passed, Logic seeing the
   plugin at all is the leading indicator that carried over correctly. If
   Logic silently omits it after a full rescan, that's a FINDING — report
   back with any error Logic shows (Plug-in Manager has a "show info" /
   crash-report affordance for failed scans).

## 4. Smoke test rows

Report **PASS / FAIL** for each row. For anything marked FINDING below,
stop and report rather than trying to work around it live.

| # | Row | What to do | Expected |
|---|-----|------------|----------|
| 1 | Insert (aumu) | New Software Instrument track → click the instrument slot → find **GentSampler** under AU Instruments → JoeyTheGent (or similar vendor grouping) → insert the default (non-multi-output) variant. | Plugin editor opens, no crash. |
| 2 | Musical Typing | No MIDI controller needed: **Window → Musical Typing** (Cmd-K), click a few keys on the on-screen/QWERTY keyboard. | Pads 1-16 (or whichever are assigned) trigger sound; this is the no-controller MIDI path — if you have a MIDI controller, that's fine too, just note which you used. |
| 3 | Finder drag (canonical) | Drag an audio file (wav/mp3/aiff, anything a few seconds to a minute long) from a **Finder window** directly onto the plugin editor. | Track loads, waveform appears in the hero view. |
| 3b | **BONUS** — Logic-browser drag | Same, but drag the audio file from **Logic's own file browser / arrange area** instead of Finder. | Note whether this works or behaves differently — different drag source, don't debug live, just report what happened. |
| 4 | BPM/key detection | After loading a track (row 3), check the header readouts. | BPM and key populate (not blank/zero). |
| 5 | Cue points | Look at the hero waveform / slice strip. | Cue markers are present (auto-detected transients), not all bunched at the start. |
| 6 | Pads play/flip | Click a few of the 16 pads directly in the plugin UI (not via MIDI). | Each pad plays its assigned slice; flipping (if you try it) changes what's mapped. |
| 7 | CPU separation (first run) | Click **SEPARATE STEMS**. **First time only:** this downloads ~1.79 GB of model weights — needs internet, will take a while depending on your connection. **While it downloads, report back what the UI shows** (a progress indicator, percentage, or just "nothing visible yet") — we need to confirm you can actually observe it working, not just guess it finished. | Download completes, then separation runs (CPU-only on mac — no GPU path), and you get 6 stems (drums/bass/vocals/guitar/piano/other — STEMS view / chips). |
| 8 | Kit save/load | Save the current pad layout as a kit (Kit menu / save icon), then load it back. | Kit file is written somewhere sensible (report the path if asked/shown), reloading restores the same pad assignments. |
| 9 | Multi-output insert variant | Remove the plugin, re-insert it, but this time **choose the multi-output variant** from Logic's instrument picker (it should offer a "GentSampler (16 outs)" or similar alternate entry alongside the plain stereo one). Then in the Mixer, use the **"+" (Create New Aux Channel)** control on the instrument channel strip to bring up the extra outputs as aux channels. | Multi-output variant is offered and inserts; aux channels appear and can be created for the extra pad outputs. **If Logic does NOT offer a multi-output variant at all, despite our CI `auval` gate passing green — that is a FINDING. Stop and report; don't try to force it.** |
| 10 | Project state save/reopen | With the plugin loaded and at least one track/pads/cues set up: **File → Save** the Logic project, **quit Logic**, relaunch, **reopen** the project. | GentSampler reappears with the same file loaded, same cues, same pad assignments — full state restored. This is AU state's first real-host exercise, so please be thorough here (check pads individually, not just that the plugin opens). |

## 5. Visual smoke (native screenshots only)

For each item below, take a **native macOS screenshot** — **Cmd-Shift-4** to
select a region (or Cmd-Shift-3 for the whole screen) — and send us the PNG
files directly. **Do NOT send a screenshot of your screen taken through
Zoom's screen-share** — Zoom's video compression will smear/blur exactly the
kind of fine detail (glow gradients, thin hairlines, small mono numerals)
we're trying to check, and we can't judge a compressed frame.

- Arc-knob glow rendering (any of the rotary knobs) — should show a soft
  glow, not a flat/dead ring or a rendering artifact.
- Hero waveform paint — the main waveform display should render cleanly,
  no missing chunks or garbled pixels.
- Mono numeric readouts (BPM, key, the small tabular numbers throughout the
  inspector) — should be in a monospace font (Menlo on mac) with digits
  lining up in neat columns, not a fallback font with ragged alignment.
- Capture the plugin window at **two sizes**: resize to roughly **1040×700**
  (natural/default size) and then shrink down to the **880×592 floor** (the
  smallest supported size) — confirm nothing is cut off, overlapping, or
  unreadable at the floor size.

## 6. Dylib-absent degradation (human half)

This one you won't normally hit — CI already tested the machine half (strip
the ORT library, confirm the plugin still loads and pluginval passes). The
human-observable question is just: **if separation or transcription ever
reports "engine unavailable" or similar instead of working**, does the UI
fail gracefully (a clear message, no crash) rather than hanging or crashing?
You're not expected to manufacture this condition — just note if you ever
see a degraded/unavailable message during normal use, and what it said.

## 7. Separation audio relay

When reporting on the separation result (row 7 above) or any other audio
quality question over Zoom, please do ONE of the following instead of just
letting Zoom's default audio compression carry it (which mangles audio
quality enough to make judgment calls unreliable):
- **Preferred:** export the separated stem WAVs and send us the files
  directly (AirDrop, file transfer, cloud link — whatever's easiest).
- **If you want to play it live on the call:** enable Zoom's **"Original
  sound for musicians"** option (Zoom Settings → Audio) before playing
  anything back — this disables Zoom's normal voice-optimized compression.
  Do not rely on Zoom's default mic/speaker path for anything we need to
  judge sonically.

## 8. Crash log export (only if something crashes)

If Logic or the plugin crashes at any point:
1. Open **Finder → Go → Go to Folder…** → paste:
   ```
   ~/Library/Logs/DiagnosticReports
   ```
2. Look for the most recent `.ips` file matching `Logic Pro` or
   `GentSampler` (sorted by date, should be right at the top).
3. Send us that `.ips` file along with a plain description of what you were
   doing right before the crash.

## 9. OPTIONAL bonus rows

These are flagged as optional — do them if you have time/interest, but
they're never required for us to call this test done.

- **Rosetta-Logic (x86_64 pass):** Quit Logic Pro. In Finder, select
  Logic Pro.app → Cmd-I (Get Info) → check **"Open using Rosetta"** → relaunch
  Logic and repeat a few of the smoke rows above (insert, play pads, one
  separation). This exercises the Intel (x86_64) half of the universal2
  build hands-on, which otherwise only gets CI/build-only coverage on your
  hardware. Uncheck "Open using Rosetta" afterward to go back to normal.
- **FL Studio mac trial (VST3 smoke):** If you're willing to install the FL
  Studio trial, copy `GentSampler.vst3` to `~/Library/Audio/Plug-Ins/VST3/`
  (same quarantine-strip step applies, substitute the `.vst3` path), rescan
  plugins in FL, and do a quick load + play-pads smoke test. This is the
  only hands-on coverage the mac VST3 build gets at all (mac VST3 is
  otherwise machine-only, via pluginval in CI) — appreciated but genuinely
  optional.

## 10. Report back to Joe

For each numbered row above (1-10 in the smoke table, plus 5-9 as
applicable): **PASS / FAIL**, plus:
- Pre-flight answers (uname -m, macOS version, disk free, Logic Pro version).
- Any error message, verbatim, and which row it happened on.
- The native screenshots from section 5.
- Separation audio (files or the Zoom "Original sound" note) from section 7.
- Any crash `.ips` file from section 8, if applicable.
- Whether you attempted either optional bonus row, and its result if so.

Joe makes the final PASS/FAIL call on the overall test — this checklist
just makes sure nothing important gets skipped or judged over a bad Zoom
frame.
