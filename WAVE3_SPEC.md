# SPEC: AUDIT WAVE 3 — the three LOW findings (PREPACKAGE_AUDIT.md #16–#18)

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `9ada7dd`.
**Provenance:** authored directly by the PLANNER (Fable, 2026-07-05).
Drafter step SKIPPED per ROUTING.md Rule 8 economics — this spec is smaller
than a drafter dispatch; the draft-then-review lever pays only on large specs.
All symbols verified at HEAD. **APPROVED by Joe 2026-07-05 ("go") — implementation authorized.**
**Order:** #17 → #16 → #18 (mechanical → mechanical → test-authoring).

## Ground rules (Waves 1–2 protocol)
- One fix = one checkpoint commit ("WAVE3 #N: ..."), PREPACKAGE_AUDIT.md
  status row in the same commit. Gates per checkpoint (VERIFY_CMD +
  LINT_CMD), FL-deploy-lock exception as before. NEVER terminate FL/any app.
- Budgets: default 3 attempts, 2 review cycles. Tier: IMPLEMENTER all three
  (#17/#16 are near-BULK mechanically but sit on IO paths; #18 is test
  authoring — a wrong assertion is caught by nothing downstream, Rule 1).

## #17 — 10-minute cap on stem-cache FLAC decode  (1st)
`doStemCacheLoadJob` (PluginProcessor.cpp:3619): `const int len = (int)
reader->lengthInSamples;` — the file's only remaining uncapped decode.
Mirror the identical pattern used at :268/:3437/:3459/:3513
(`maxLen = (int64)(reader->sampleRate * 600.0); len = jmin(...)`).
**AC:** the five decode sites are now uniform (reviewer reads them side by
side); nothing else changes; gates green.

## #16 — create Documents\GentSampler before the first heartbeat  (2nd)
`doStemJob` (PluginProcessor.cpp:~1317-1326): `logFile =
gentDir.getChildFile("stems_log.txt")` + the `heartbeat` lambda's first call
fire before anything guarantees `gentDir` exists — on a fresh install the
first heartbeat write silently no-ops (juce::File::replaceWithText fails
into the void), so the "loading models…" progress file is missing exactly
when a first-run user needs it. Fix: `gentDir.createDirectory();` once,
immediately before the heartbeat lambda's definition (idempotent, cheap,
worker thread). Verify no OTHER first-write-before-create exists in
doStemJob's path (the later cache/report writers already create their dirs —
implementer confirms, notes findings, fixes ONLY if the same one-line shape).
**AC:** fresh-profile first run produces stems_log.txt from the first
heartbeat (Joe smoke: delete Documents\GentSampler, separate, file appears
immediately); gates green.

## #18 — doctest coverage for classifySlice branch 1c  (3rd)
`gent::classifySlice` (EngineMath.h): the ambiguous-stems fallthrough —
`hasStems == true` but NEITHER drums-dominant NOR tonal-dominant (1c) — has
zero test coverage; it routes into the no-stems spectral tree with
`kThreshNoStems`. Add cases to tests/ClassifierTests.cpp (same file
conventions: table-relative values ONLY, never copied literals; follow the
existing per-branch naming):
1. hasStems=true, stemShare split below BOTH dominance thresholds
   (e.g. drums = t.drumsDominant - margin, tonal-family likewise), spectral
   features shaped for KICK per kThreshNoStems → expect KICK via the
   fallthrough (proves 1c reaches the no-stems tree with the right preset).
2. Same ambiguous stems, spectral features shaped for TONAL per
   kThreshNoStems → expect TONAL.
3. Same, spectral features weak everywhere → expect OTHER (min-confidence
   demotion still applies through the fallthrough).
4. Contrast pin: drums CLEARLY dominant (t.drumsDominant + margin) with the
   SAME spectral features as case 2 → must NOT take the fallthrough
   (result differs from case 2's), proving 1a/1c divergence.
**AC:** ≥4 new cases, all table-relative, all passing; existing cases
untouched; ctest count grows accordingly; gates green.

## File plan
| File | Fix |
|---|---|
| Source/PluginProcessor.cpp | #17, #16 |
| tests/ClassifierTests.cpp | #18 |
| PREPACKAGE_AUDIT.md | status rows per fix |
| CLAUDE.md | wave close only (lead) |

## Out of scope
Everything already landed (Waves 1–2). No classifier BODY changes (#18 is
tests only — if a test exposes a real defect in 1c, STOP and report; do not
fix the classifier). No new deps, schema changes, or harnesses.

## Verification (every checkpoint)
```
bash .claude/hooks/build_test.sh
bash .claude/hooks/pluginval_gate.sh
```

## Joe's manual pass (wave close)
1. Delete Documents\GentSampler, run a separation → stems_log.txt appears
   from the first moment (#16).
2. Reopen a stem-cached project → stems restore as before (regression smoke
   on #17's cap).
(#18 is machine-verified by ctest — no manual step.)

## Record
| Fix | Checkpoint commit | Reviewer verdict | Joe |
|---|---|---|---|
| #17 | | | |
| #16 | | | |
| #18 | | | |
