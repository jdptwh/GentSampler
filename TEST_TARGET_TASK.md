# SPEC: Minimal unit-test target for GentSampler engine core

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ HEAD 9578802
**Origin:** Joe, 2026-07-02 — first test target for the project. Logic only, fast, CTest-wired into gate.sh.

**Objective** — Add the project's first CTest-wired, logic-only unit-test target covering slice-window math, snap math, trigger-state transitions, and stem-mask semantics, running green in seconds as a new gate in gate.sh, with zero plugin behavior change.

## Framework decision: doctest v2.4.12, vendored single header

Catch2 v3 is no longer header-only — it is a multi-TU compiled library that would need its own `FetchContent` (a new configure-time network fetch and a measurable add to the one-click build); Catch2 v2's single header is EOL and notoriously slow to compile on MSVC. doctest is a single ~180 KB MIT header, the fastest-compiling C++ test framework in common use, fully C++17/MSVC-clean, and unaffected by `/utf-8` (we set it on the test target anyway). Vendoring it at `tests/vendor/doctest.h` means **zero new FetchContent, zero network dependency, and no way for a failed fetch to brick the one-click build** — the test target is pure additive CMake guarded by a file-existence check.

## Ground rules

1. **Logic only.** The test binary links **no JUCE modules**, instantiates no processor/editor, renders no audio. Everything under test is a pure function in a new header, `Source/EngineMath.h`, that uses only `<cstdint> <cstdlib> <algorithm> <vector> <array>`.
2. **Extraction is mechanical and behavior-identical.** Each production function keeps its signature, locking, atomics, and guards; only its arithmetic/decision core moves to `EngineMath.h` and is called back. `juce::jmax/jmin/jlimit` on `int` become `std::max/std::min/std::clamp` **inside EngineMath.h only** (identical semantics for the int ranges involved). Every such move is listed below; **all of them together constitute one reviewer-gated refactor (T6)**.
3. **Tests encode current behavior**, including behaviors Joe may later want changed (they get flagged, not "fixed").
4. Plugin targets untouched: `VST3_AUTO_MANIFEST FALSE`, `COPY_PLUGIN_AFTER_BUILD FALSE`, both POST_BUILD copy steps, ORT block — not edited.

## Design source

None — no UI change. **UI acceptance criteria: N/A** (no UI is added or altered; pluginval at strictness 5 remains the editor/lifecycle gate and must still pass unchanged).

## File plan

| File | Change |
|---|---|
| `Source/EngineMath.h` | **NEW** — namespace `gent`: pure functions extracted below; no JUCE includes |
| `Source/PluginProcessor.h` | MOD — `kOpenSlice` becomes `= gent::kOpenSlice`; `setPadStemBit` (h:232-239) delegates mask math to `gent::stemMaskWithBit`; include EngineMath.h |
| `Source/PluginProcessor.cpp` | MOD — `setCue` (409-410), `setCueEnd` (417-422), `getEffectiveCueEnd` (425-454), `nearestTransient` (300-311), `snapCursor` (540-563), `startVoice` (1481-1494, 1503-1513, 1609), `releaseVoices` (1640-1643) delegate their decision cores to `gent::` functions |
| `Source/PluginEditor.h` | MOD — `applyEndHandleDrag` (61-69), `resolveSnap` (138-146), `padMapHue` (38-45), `padSourceColour` (23-30) delegate to `gent::` functions |
| `CMakeLists.txt` | MOD — append guarded test block at end of file: `enable_testing()`, `GentSamplerTests` target, `add_test` |
| `tests/vendor/doctest.h` | **NEW** — vendored doctest v2.4.12, byte-identical upstream copy |
| `tests/TestMain.cpp` | **NEW** — `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` + include |
| `tests/SliceWindowTests.cpp` | **NEW** — T2 |
| `tests/SnapTests.cpp` | **NEW** — T3 |
| `tests/TriggerTests.cpp` | **NEW** — T4 |
| `tests/StemMaskTests.cpp` | **NEW** — T5 |
| `.claude/hooks/gate.sh` | MOD — new `tests` gate between build and pluginval (exact edit in T1) |
| `CLAUDE.md` | MOD — "Tests:" convention line, "Verification command" note, "Current state" |

---

## T1 — Scaffold: CMake target, CTest, gate.sh, CLAUDE.md

**CMake** (append at end of `CMakeLists.txt`; touches no existing lines):

```cmake
# ---- unit tests (logic-only; no JUCE link; see TEST_TARGET_TASK.md) ----
option(GENT_BUILD_TESTS "Build EngineMath unit tests" ON)
if(GENT_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/vendor/doctest.h")
    enable_testing()
    add_executable(GentSamplerTests
        tests/TestMain.cpp
        tests/SliceWindowTests.cpp
        tests/SnapTests.cpp
        tests/TriggerTests.cpp
        tests/StemMaskTests.cpp)
    target_include_directories(GentSamplerTests PRIVATE Source tests/vendor)
    target_compile_features(GentSamplerTests PRIVATE cxx_std_17)
    target_compile_options(GentSamplerTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME EngineMath COMMAND GentSamplerTests)
    set_tests_properties(EngineMath PROPERTIES TIMEOUT 60)
endif()
```

**gate.sh** — insert after the `VERIFY_CMD` line (line 17):

```bash
TEST_CMD="${CLAUDE_TEST_CMD:-ctest --test-dir build -C Release --output-on-failure --no-tests=error}"
```

and after `run_gate "build" "$VERIFY_CMD"`, before the pluginval block:

```bash
run_gate "tests" "$TEST_CMD"
```

Update the gate list comment at the top to show `GATE 1 build / GATE 2 tests / GATE 3 pluginval / GATE 4 lint / GATE 5 ui`. Rationale for order: ctest is ~seconds; pluginval is ~minutes — fail fast on logic. `--no-tests=error` makes a silently-missing test target a hard failure (the VS generator's ZERO_CHECK re-runs configure on the first build after this change, so the target appears without hand-touching `build/`).

**doctest acquisition:** network fetches are on the agent deny list. The implementer must NOT curl it; Joe (or the lead, with explicit permission) downloads `doctest.h` from the doctest v2.4.12 release and drops it at `tests/vendor/doctest.h`. If it is absent when implementation starts, **stop and ask** — do not substitute a FetchContent.

**CLAUDE.md edits:**
- Conventions → `- Tests: ctest unit tests (tests/, doctest, logic-only — pure functions in Source/EngineMath.h). Run: ctest --test-dir build -C Release --output-on-failure. gate.sh runs them as GATE 2, after build, before pluginval.`
- Verification command line gains: `then ctest --test-dir build -C Release --output-on-failure`
- Current state updated on completion.

**T1 acceptance criteria**
1. `cmake --build build --config Release --parallel` succeeds and now also builds `GentSamplerTests` (build log contains `GentSamplerTests.vcxproj`).
2. `ctest --test-dir build -C Release --output-on-failure --no-tests=error` exits 0.
3. Total ctest wall time < 10 s (read from ctest's own timing output).
4. With `tests/vendor/doctest.h` renamed away and a fresh configure, the plugin still configures and builds (guard works) — restore afterwards.
5. `bash .claude/hooks/gate.sh` runs gates in order build → tests → pluginval and prints `[gate:tests] PASS`.
6. `git diff CMakeLists.txt` shows only the appended block; no line above it changed.

---

## T2 — Slice window math (`tests/SliceWindowTests.cpp`)

**Extractions into `gent::` (from cited lines):**
- `constexpr int kOpenSlice = -2;` — `GentSamplerAudioProcessor::kOpenSlice` (PluginProcessor.h:135) becomes `= gent::kOpenSlice`.
- `int resolveCueEndEdit(int samplePos, int cue)` — from `setCueEnd` body (PluginProcessor.cpp:417-422): `<0 → -1`; `≤ cue+32 → kOpenSlice`; else `samplePos`.
- `struct CueEditResult { int cue, end; };  CueEditResult applyCueEdit(int samplePos, int currentEnd)` — from `setCue` (PluginProcessor.cpp:407-410): `cue = max(0, samplePos)`; end auto-clears to `-1` iff `end >= 0 && end <= samplePos + 32`.
- `int effectiveCueEnd(int cue, int end, int len, bool sliceMode, const std::array<int,16>& allCues)` — from `getEffectiveCueEnd` (PluginProcessor.cpp:425-454), including the `len < 2 → max(0,len-1)`, `cue < 0 → -1`, `kOpenSlice → len-1`, `end > cue → min(end, len-1)`, and slice-mode next-cue scan branches.
- `int resolveEndDragTarget(int cue, int proposedEnd, int collapseTolSamples)` — from `applyEndHandleDrag` (PluginEditor.h:61-69): `proposed ≤ cue+tol → cue` (which `resolveCueEndEdit` then collapses to OPEN), else `max(cue+33, proposedEnd)`.

**Acceptance criteria (each is a named TEST_CASE):**
1. `resolveCueEndEdit(532, 500) == gent::kOpenSlice` (cue+32 collapses); `resolveCueEndEdit(533, 500) == 533` (cue+33 is the minimum real window); `resolveCueEndEdit(-1, 500) == -1`.
2. `applyCueEdit(980, 1000)` clears end to `-1` (start pushed to within 32 of end); `applyCueEdit(967, 1000)` keeps end 1000 (boundary: 1000 > 967+32); `applyCueEdit(-50, x).cue == 0` (clamp to sample start).
3. `effectiveCueEnd`: OPEN → `len-1`; explicit end beyond sample → clamped `len-1`; auto + sliceMode → smallest cue strictly greater than this pad's cue across all 16; auto + sliceMode + no later cue → `len-1`; `cue = -1` → `-1`; `len = 0` → `0`.
4. `resolveEndDragTarget(1000, 1150, 200) == 1000` (within tolerance → collapse); `resolveEndDragTarget(1000, 1201, 200) == 1201`; `resolveEndDragTarget(1000, 1010, 0) == 1033` (min-window floor).
5. **Invariant property test:** 1000 pseudorandom (fixed-seed) sequences of `applyCueEdit`/`resolveCueEndEdit`/`resolveEndDragTarget` over a fake 16-pad state never produce a stored state where `end >= 0 && end <= cue + 32` (cue can never pass end), and `effectiveCueEnd - cue` is either the sentinel path or `>= 1`; `len == end - cue` holds by construction wherever a real window exists.

---

## T3 — Snap capture math (`tests/SnapTests.cpp`)

**Extractions:**
- `int nearestTransientIn(const std::vector<int>& ts, int sourcePos, double sampleRate)` — the loop + 50 ms cap from `nearestTransient` (PluginProcessor.cpp:300-311). Production keeps the SpinLock copy and `getSource()` fallback-`44100.0` and calls this.
- `int applySnapThreshold(int proposed, int candidate, double sppNow)` — the 6-px capture test from `resolveSnap` (PluginEditor.h:144-145): snap iff `|cand − proposed| ≤ 6·sppNow`. Production `resolveSnap` keeps the `snapEnabled`/`altDown` early-out and the grid-vs-transient *selection* and calls this.
- `int selectSnapCursor(int pos, bool hasGrid, int gridCand, const std::array<int,16>& cues, int transientCand, double sampleRate)` — the candidate competition from `snapCursor` (PluginProcessor.cpp:540-563), including the grid-free 50 ms stay-free rule.

**DISCREPANCY FLAG for Joe (do not "fix" in this task):** `resolveSnap` has **no** grid/transient competition — grid is used *iff* `gridStepSamples() > 0`, transients only otherwise (PluginEditor.h:142-143). The nearest-point competition Joe described lives in `snapCursor` (grid line vs all 16 placed cues vs transients). Tests encode both actual behaviors.

**Acceptance criteria:**
1. `applySnapThreshold(p, p+60, 10.0) == p+60` (exactly 6·spp engages); `applySnapThreshold(p, p+61, 10.0) == p` (never beyond); `applySnapThreshold(p, p+1, 0.0) == p` (zero-spp degenerate).
2. Alt-bypass and snap-disabled early-outs remain in the 4-line production wrapper; the test file contains a comment citing PluginEditor.h:140-141 stating they are covered by reviewer inspection, not unit test (no processor construction).
3. `nearestTransientIn({44100}, 44100+2204, 44100.0) == 44100` (within 50 ms cap: maxDist = 2205); `…+2206 …` returns the input unchanged (beyond cap); empty vector returns input; equidistant transients → the **earlier vector element** wins (strict `<`).
4. `selectSnapCursor`: with grid, nearest of {gridCand, 16 cues} wins regardless of distance (grid path has no 50 ms bail); without grid, a transient/cue farther than 0.05·sr from `pos` → `pos` unchanged (stay-free rule); a placed cue at d=50 beats a grid line at d=100.
5. Effective transient capture in `resolveSnap` = min(6-px threshold, 50 ms cap): composition test `applySnapThreshold(p, nearestTransientIn(...), spp)` with a transient at 40 ms and spp large → snaps; transient at 60 ms → never snaps regardless of spp.

---

## T4 — Trigger/playback state transitions (`tests/TriggerTests.cpp`)

**Finding:** the state machine is **not** callable without audio — it is inlined in `startVoice` (PluginProcessor.cpp:1481-1521) and `releaseVoices` (1634-1650), tangled with voice-pool allocation. Minimal extraction (pure predicates, mode encoding `0 gate / 1 one-shot / 2 latch`):

- `bool latchPressTurnsOff(int mode, bool kbMode, bool padSounding)` — from 1484-1494: `!kbMode && mode == 2 && padSounding`. ("padSounding" = any active voice on the pad with `state != 2` — that scan stays production-side; the predicate takes the boolean.)
- `bool voiceGateFlag(int mode, bool kbMode)` — from line 1609: `kbMode || mode == 0`.
- `bool chokeSilences(int myChoke, int triggerPad, bool otherActive, int otherState, int otherPad, int otherChoke)` — from 1503-1513.
- `bool releaseApplies(bool vActive, int vPad, int vNote, bool vGate, int vState, int pad, int note, bool quick, bool onlyGate)` — the four filter lines of `releaseVoices` (1640-1643).

**Acceptance criteria:**
1. LATCH: `latchPressTurnsOff(2,false,true) == true` (tap-off); `(2,false,false) == false` (tap-on); `(2,true,true) == false` (keyboard mode never latch-toggles); `(0|1, *, *) == false`.
2. GATE flag: `voiceGateFlag(0,false)` true, `voiceGateFlag(1,false)` false, `voiceGateFlag(2,false)` false, `voiceGateFlag(anything,true)` true.
3. ONE-SHOT survives key-up: `releaseApplies(active, pad, -1, /*vGate*/false, 0, pad, -1, false, /*onlyGate*/true) == false` — mirrors `handleNoteOff`'s `onlyGate=true` call (PluginProcessor.cpp:1675) leaving one-shot/latch voices playing.
4. Retrigger: same-pad press path (`releaseVoices(pad,-1,true,false)`, line 1497) → `releaseApplies(..., quick=true, onlyGate=false)` true even when `vState == 2` (quick fade overrides an in-progress release); non-quick on `vState == 2` is false.
5. CHOKE: `chokeSilences` false when `myChoke == 0`; false for the triggering pad itself; false when the other voice is already releasing (`otherState == 2`); true iff groups match, pads differ, voice active-and-sounding. Exhaustive over choke groups {0,1,2} × pads {0,1}.
6. Composition scenario tests (plain structs, no JUCE): a scripted press/release/press sequence per mode asserting the resulting predicate outcomes match the table: GATE = sound while held; ONE-SHOT = plays to end, retrigger quick-fades old voice; LATCH = press-on, press-off.

---

## T5 — Stem-source switching (`tests/StemMaskTests.cpp`)

**Extractions:**
- `std::uint8_t stemMaskWithBit(std::uint8_t m, int stem, bool on)` — from `setPadStemBit` (PluginProcessor.h:232-239) incl. the `& 0x3F` clamp; the pad/stem range guard stays production-side.
- `int singleStemIndex(std::uint8_t m)` — the `m != 0 && (m & (m-1)) == 0` classification from `padMapHue`/`padSourceColour` (PluginEditor.h:26-28, 41-43); returns 0-5 or −1 for FULL/multi. `padMapHue` becomes `k >= 0 ? stemColour(k) : Theme::fullStem`; `padSourceColour` becomes `k >= 0 ? stemColour(k) : padColour(i)`.
- `float stemGainFor(std::uint8_t pmask, int k, float bleedParam, bool globalAudible)` — the per-voice gain table from processBlock (PluginProcessor.cpp:2422-2437): FULL→1, selected bit→1, unselected→`clamp(bleed,0,1)·0.5`, global-inaudible→0.

**"Valid state" definition (from code, cite in test comments):** the mask is one `std::atomic<uint8_t>` per pad (PluginProcessor.h:439); the audio thread reads it **once per block** (PluginProcessor.cpp:2422) and ramps `v.sg[]` toward the derived targets — so mid-playback validity = (a) every stored value has bits only in `0x3F`, and (b) every mask×stem×bleed combination yields a gain in [0,1]. No multi-word atomicity requirement exists.

**Acceptance criteria:**
1. Bit ops: `stemMaskWithBit(0,2,true)==0x04`; clearing returns to 0 (FULL); `stemMaskWithBit(0xFF,5,true)==0x3F` (out-of-range bits stripped); set is idempotent; set-then-clear round-trips for all 6 stems.
2. FULL-neutral rule: `singleStemIndex(0)==-1`, `(0x01)==0`, `(0x20)==5`, `(0x03)==-1`, `(0x3F)==-1` — single-bit → stem hue index, FULL/multi → −1 (neutral).
3. Gain table: mask 0 → 1.0 for all six stems; mask 0x01 → k=0 gets 1.0, k=1..5 get `bleed·0.5`; bleed 2.0 and −1.0 clamp to 0.5 and 0.0; `globalAudible=false` → 0 always.
4. Validity property: exhaustive — all 64 legal masks × 6 stems × bleeds {−1, 0, 0.5, 1, 2} × audible {t,f} produce gains in [0,1]; 1000 fixed-seed random `stemMaskWithBit`/reset-to-0 sequences never leave the [0, 0x3F] range.

---

## T6 — Refactor review gate (REQUIRED — refactors happened)

Extractions in T2-T5 modify production code, so a reviewer pass is mandatory. Reviewer (GATE tier) verifies, with verdict format `PASS`/`FAIL` + `file:line — issue — fix`:

1. Every delegation is behavior-identical: same comparisons, same boundary constants (+32, +33, 6·spp, 0.05·sr, 0x3F, quick-fade override), same sentinel values; `std::max/min/clamp` substitutions only on `int` paths.
2. No locking, atomic, or guard moved out of production functions into EngineMath.h.
3. No file outside the file plan touched; no plugin-target CMake line changed; `EngineMath.h` includes no JUCE header.
4. Diff readable; test cases actually assert the cited line's behavior (spot-check against PluginProcessor.cpp:409, 417-422, 1481-1494, 1640-1643; PluginEditor.h:61-69, 138-146).

## Regression fence

- `cmake --build build --config Release --parallel` — plugin still builds clean.
- pluginval strictness 5 still passes (behavior-identical claim's machine check for the plugin lifecycle).
- No change to any audio output path beyond mechanical delegation; `processBlock` hot loop (2430-2437) delegation must compile inline (header function) — reviewer confirms no allocation/locking added on the audio thread.
- gate.sh with the new tests gate still exits 0 end-to-end.

## Verification commands

```
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
bash .claude/hooks/gate.sh
```

## Out of scope — do NOT touch

`build/` (generated), ORT/CUDA blocks in CMakeLists.txt, `VST3_AUTO_MANIFEST`, `COPY_PLUGIN_AFTER_BUILD`, both POST_BUILD copy commands, `StemSeparator.*`, `Transcriber.*`, `ModelDownloader.*`, `Theme.h`, any APVTS parameter layout, any behavior "improvement" the tests reveal (flag, don't fix), no new FetchContent, no JUCE link in the test target, no upgrade of anything pinned.

## Tier assignment (ROUTING.md Rule 1)

| Item | Tier | Justification |
|---|---|---|
| T1 CMake + gate.sh + CLAUDE.md | WORK (sonnet) | A wrong gate.sh silently passes — nothing machine-catches a gate that skips itself. Not BULK-eligible. |
| doctest.h vendoring | Human (Joe/lead) | Network fetch is deny-listed for agents; byte-identical drop-in, verified by tests compiling. |
| T2-T5 extraction + tests | WORK (sonnet) | Behavior-identical refactor of production logic is judgment-shaped; the compiler cannot catch a subtly-changed boundary constant. |
| T6 review | GATE (Fable) | Pre-merge review of a production refactor with audio-thread blast radius. |

No BULK assignments: every item either touches production logic or gate plumbing where the machine check cannot catch the failure class.

## Risks

1. **Silent behavior drift in extraction** (off-by-one on +32/+33, clamp-order swap) → tests are written against the *cited current lines* first, T6 reviewer diffs constant-by-constant, pluginval must still pass.
2. **Stale build tree lacks the test target** → VS generator's ZERO_CHECK reconfigures on first build; `--no-tests=error` turns any residual miss into a loud failure instead of a fake green.
3. **Test target breakage blocks the plugin build gate** (it builds in the same `cmake --build` pass) → intended coupling; the guard `if(EXISTS tests/vendor/doctest.h)` plus `GENT_BUILD_TESTS=OFF` gives an escape hatch without touching plugin targets.
4. **`std::clamp` UB if hi < lo** (vs `jlimit`'s assert) → none of the extracted call sites can reach hi < lo (each has a preceding guard); reviewer verifies per site.
5. **doctest.h missing at implementation start** → implementer stops and asks (no guessing, no substitute fetch).

## Definition of Done

- [ ] `cmake --build build --config Release --parallel` green
- [ ] `ctest --test-dir build -C Release --output-on-failure --no-tests=error` green, < 10 s
- [ ] pluginval strictness 5 green (unchanged plugin behavior)
- [ ] All T1-T5 acceptance criteria checked off
- [ ] T6 reviewer verdict PASS on the production refactor
- [ ] No files touched outside the file plan
- [ ] CLAUDE.md: "Tests:" line, verification command, and "Current state" updated (Tests are no longer "none")

---

*Spec authored by planner (Fable) 2026-07-02; HEAD at authoring: 9578802. All symbols/lines verified against code before authoring. Discrepancy flagged: the grid-vs-transient nearest-point competition Joe described exists in snapCursor (cursor placement), not resolveSnap (handle drags) — tests encode both actual behaviors.*
