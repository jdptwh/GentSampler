# GentSampler v1.1

A 16-pad flip sampler VST3 built around the fastest workflow in sampling: drop a track in, it detects BPM and key, auto-sets cue points on transients, time-stretches to your project tempo, and lets you shift master pitch without changing speed. Then — unlike the big-name competition — it lets you get your work OUT: drag slices out as WAVs, drag your pad performance out as MIDI, export full kits, and keep a clearance-ready Flip Log. Original code, no account, no online activation, no subscription. Powered by the MIT-licensed Signalsmith Stretch engine.

## Features

**Core flip workflow**
- Drag & drop or LOAD — wav, mp3, aiff, flac, ogg (capped at 10 minutes)
- Auto BPM detection (spectral-flux onset autocorrelation, 60–180 BPM)
- Auto key detection (chromagram + Krumhansl-Schmuckler, e.g. "F# Min")
- Tempo sync — sample time-stretches to follow your FL project tempo
- Master pitch ±12 st, length-preserving (true stretch, not repitch)
- 16 cue pads: AUTO-SLICE (transients) or GRID menu (16 equal / every beat / every 2 beats / every bar)
- Click/drag the waveform to move the selected pad's cue
- Per-pad: pitch ±24 st, level, attack, release, CRUSH, play mode (GATE / ONE-SHOT / LATCH), and STOP AT NEXT CUE
- GATE sounds only while the key is held (the default). ONE-SHOT fires the slice and stops on its own. LATCH toggles: press to lock a chop playing, press again to stop it — lock a loop on one pad while you gate others over it
- Keyboard mode: play the selected pad chromatically (C3 = original)
- MIDI notes 36–51 (C1–D#2) = pads 1–16, FPC's default layout

**CRUSH (per pad)**
SP-1200-style character: sample-rate hold + bit-depth reduction, dialed from clean (0%) to dusty 12-bit-and-below grit (100%). Applied per pad, baked into exports.

**Getting your work out (the stuff other samplers won't do)**
- **PAD WAV chip** — drag it out of the plugin and drop the selected pad's slice (pitch/level/crush applied) anywhere: FL playlist, a folder, another plugin
- **REC MIDI → MIDI chip** — hit REC, play your pads, hit STOP, then drag the chip into the FL piano roll as a .mid of your performance
- **EXPORT KIT** — writes 16 numbered WAV slices + GentSampler_Kit.mid (note map) + FlipLog.txt to a folder of your choice
- **SAVE KIT / LOAD KIT** — .gentkit preset files (sample path, cues, all knobs); drag a .gentkit onto the window to load it
- **Flip Log** — a clearance-ready text record of exactly which source file you flipped, where each pad sits in the original timeline (mm:ss.mmm), and how it was pitched/processed

**Per-pad outputs**
The plugin exposes a Main stereo output plus 16 optional per-pad stereo outputs. Enable the extra outputs in FL Studio's wrapper (Processing > connections / output routing) to mix, EQ, and effect each pad on its own mixer track. Pads route to Main unless their bus is enabled.

**Full state recall** — everything saves with your FL project.

## One-time setup (about 10 minutes, mostly download time)

Open **PowerShell** and run:

```powershell
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

That last one installs Microsoft's command-line C++ compiler — no Visual Studio IDE, no bloat.

## Build

Double-click **build.bat** (right-click → Run as administrator to install system-wide). First build downloads JUCE and takes a few minutes; after that it's fast. The script installs to `C:\Program Files\Common Files\VST3` (or `Documents\VST3` without admin — it tells you if you need to add that path in FL).

## In FL Studio

1. Options → Manage plugins → Find more plugins
2. GentSampler shows up under Installed → Generators → VST3
3. Drop it on a channel, drag a track in, hit pads

**Workflow tip:** TEMPO SYNC on, drop a song in, every pad is on-grid at your project tempo. MASTER PITCH moves the flip into your key (the KEY readout tells you where it starts). Twist CRUSH for instant SP dust. When the chop is right, drag the PAD WAV chip into your playlist or EXPORT KIT and you're done.

## Honest engineering notes

- Per-pad pitch is classic repitch (MPC style — pitch up plays slightly faster). Master pitch IS length-preserving. Formant-preserving per-pad stretch and AI stem separation are the v2 features.
- Tempo changes re-render in the background; a big jump on a long sample takes a moment, and sounding pads cut over to the new render when it lands.
- Automating master pitch triggers re-renders — sound-design move, not a per-note move (use per-pad pitch or keyboard mode for that).
- MIDI capture timestamps at audio-block resolution against a free-running clock; quantize in the piano roll if you want it tighter.

## Troubleshooting

- **"Configure failed"** — Build Tools install didn't finish, or reopen the window so PATH refreshes. Re-run setup step 3.
- **Signalsmith API errors** — engine is fetched from its main branch; if it drifts, pin a release tag in `CMakeLists.txt`.
- **Plugin not in FL** — rescan (Find more plugins); if installed to `Documents\VST3`, add that folder in Options → File settings.
- **Per-pad outputs don't appear** — enable the extra output buses in FL's plugin wrapper processing tab; FL only shows buses you activate.
- **Any compiler error** — paste it back to Claude; usually a one-line fix.

## Legal

Original implementation inspired by a workflow — no Serato code, branding, or proprietary tech (Pitch 'n Time is replaced by Signalsmith Stretch, MIT). Features and workflows aren't copyrightable. The Flip Log is documentation, not legal advice: releasing music that samples someone else's copyrighted recording requires clearance of both the master and the composition.
