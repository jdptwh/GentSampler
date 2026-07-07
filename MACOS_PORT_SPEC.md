# MACOS_PORT_SPEC.md — macOS Port

> STATUS: **APPROVED by Joe 2026-07-07 + AMENDMENT A1 (planner, 2026-07-07: remote-friend model, CI build machine, quarantine recipe) + AMENDMENT A2 (planner, 2026-07-07: friend's DAW is LOGIC PRO — AU is the hands-on validation format, auval moves to the FIRST CI run, mac VST3 accepted at pluginval-only coverage). Phase 1 EXECUTES NOW (unaffected by A2); Phase 2 gated on Joe's private-GitHub push. JOE-DECISIONS 1/2/4/5 RESOLVED (see below); 3 remains open (ship gate).** Drafted by spec-drafter; corrected per planner review (fail-open ensureOrtLoaded stub, Consolas font, ORT mac-binary facts, signing sequencing, AU staging).

## SPEC: macOS PORT

**Objective** — Produce working **universal2 (arm64 + x86_64)** VST3 + AU builds of GentSampler on macOS — pluginval-clean (VST3) and auval-clean (AU) in CI from the first run, human-validated hands-on in **Logic Pro (AU)** on the remote tester's Mac (Amendment A2) — with CPU-only separation matching the Windows shipping path, graceful engine-unavailable degradation matching Windows, and the Windows build byte-for-byte unregressed.

**File plan**

- `Source/StemSeparator.cpp` (modify — the core task):
  (a) Hoist `ortInitLock` / `tried` / the early-out (lines 77-82) OUT of `#if JUCE_WINDOWS` so all platforms serialize ORT init identically.
  (b) **FIX the fail-open stub:** the current non-Windows branch (`#else return true;`, lines 178-180) reports ORT ready without loading anything or calling `Ort::InitApi` — any subsequent `Ort::` call on mac dereferences a null API table. Fail CLOSED (`return false`) on any path where the loader hasn't succeeded.
  (c) Mac loader (authored Phase 1, compiled/verified Phase 2): `#elif JUCE_MAC` + `<dlfcn.h>`: `dladdr((void*)&ensureOrtLoaded, &info)` to locate the plugin binary (direct analog of `GetModuleHandleExW` FROM_ADDRESS) → resolve `libonnxruntime.1.18.1.dylib` bundled inside the .vst3/.component (`Contents/Frameworks/` preferred; binary-sibling acceptable) → `dlopen(absolutePath, RTLD_NOW | RTLD_LOCAL)` → `dlsym("OrtGetApiBase")` → identical `GetApi`/`Ort::InitApi` flow. Absolute-path dlopen = no install_name/rpath surgery. No CUDA analog on mac — the `kEnableCuda` block stays Windows-only.
  (d) `makeSession` path branches (320-324, 371-375) — verify-only, already correct.
- `Source/Transcriber.cpp` — verify-only; already branches at 91-97; relies on `gentEnsureOrtLoaded()`, which (b) makes honest on mac.
- `Source/Theme.h:72` (modify) — `juce::Font("Consolas", ...)`: Consolas does not exist on macOS; silent fallback breaks the E2 mono/tabular numeric alignment (15 inspector readouts + header). Platform-select: Consolas on Windows (byte-identical rendering), `"Menlo"` on mac.
- `Source/Theme.h:323-342` (glowSprite) — verify-only; the intentional leak stays on ALL platforms (never convert back to a by-value static).
- `CMakeLists.txt` (modify): `if(APPLE)` branch selecting `onnxruntime-osx-universal2-1.18.1.tgz` (official 1.18.1 release asset — see Settled facts) as the FetchContent URL; guard both POST_BUILD DLL-copy commands (lines 119-133) `if(WIN32)` and add a mac equivalent placing the ORT dylib inside the bundle; the deploy step (149-158) is inert on mac (`IS_DIRECTORY "C:/..."` is false → SKIPPED — verified logic, confirm the message at first mac configure); `FORMATS` gains `AU` under `if(APPLE)` (JUCE derives `AU_MAIN_TYPE` from `IS_SYNTH TRUE` — no new flags expected; `Jtgt`/`Gsmp` codes are AU-valid; AU builds from the FIRST mac CI run — the VST3-first staging is dissolved, Amendment A2: Logic/AU is the only hands-on host); `CMAKE_OSX_ARCHITECTURES "arm64;x86_64"` + deployment target (default 11.0; implementer confirms the ORT dylib's min target via `otool -l`).
- `.claude/hooks/build_test.sh` + `.claude/hooks/pluginval_gate.sh` (modify — **Phase 2 first work item**): OS-branch on `uname` INSIDE the existing scripts (Rule 10 — `agent.config` stays untouched, one source of truth): mac configure/generator, no `taskkill`, mac pluginval binary/paths, plus an auval step once AU exists. Do NOT create parallel `_mac` scripts.
- `.github/workflows/macos-build.yml` (NEW — Phase 2 first work item, together with the hooks OS-branch; may be AUTHORED during Phase 1 but is only satisfiable after Joe's push — the gate is the first green CI run): macos-14 (arm64) runner; a THIN WRAPPER that runs the SAME two hooks (Rule 10 — gate content stays in the hooks, never duplicated into YAML). Required content in criterion 7.
- `MACOS_TEST_CHECKLIST.md` (NEW — Phase 2, authored before the first friend session; repo root; modeled on INSTALLER_TEST_CHECKLIST.md / CLEAN_MACHINE_CHECKLIST.md; host = LOGIC PRO, AU — Amendment A2): pre-flight (friend's `uname -m`, macOS version, free disk ≥ 5 GB, Logic Pro version — confirm Logic Pro, NOT GarageBand: GarageBand hosts AU but has no multi-output and would silently shrink criterion 12; any Mac running current Logic clears the 11.0 deployment target automatically); install path (`~/Library/Audio/Plug-Ins/Components` primary; `VST3` only for the optional bonus row); the MANDATORY quarantine-strip step on the .component (criterion 15); Logic AU rescan step (restart Logic; if the plugin is absent, Plug-in Manager full rescan — Logic re-runs auval at scan, so CI auval green is the leading indicator it appears at all); the criterion-12 smoke rows incl. Musical Typing (Cmd-K) as the no-controller MIDI path, the multi-output insert ceremony, the Logic-project state save/reopen row, and the two OPTIONAL bonus rows (Rosetta-Logic x86_64 pass; FL Studio mac trial VST3 smoke); first-run model download (criterion 13); the dylib-absent degradation behavior check (criterion 10, human half); crash-log export instructions (`~/Library/Logs/DiagnosticReports`, .ips files). PRIVATE-testing framing throughout — this document never becomes public-facing install guidance.
- `tests/PathGuardTests.cpp` — verify-only; fixtures exercise separator-agnostic `gent::pathIsWithin` (EngineMath.h:1110-1139, planner-verified portable); add one POSIX-path fixture case.
- `README.md` — mac build/support status only once real (no speculative claims).
- `CLAUDE.md` — "Current state" updates at each checkpoint per Definition of Done.

**Acceptance criteria**

*Phase 1 (Windows box — EXECUTES NOW; Amendment A1 dissolved the branch-(a)/(b) hardware condition: CI is the mac compiler, and it back-checks Phase-1 code as soon as the repo is pushed):*
1. `ensureOrtLoaded()` refactored per file plan (a)-(c): lock/tried hoisted; mac branch authored; **no code path on any platform returns true without a successful load + `Ort::InitApi`** — the fail-open stub is eliminated. Checkable by reading + grep.
2. Windows Release build + `ctest --test-dir build -C Release --output-on-failure` green, test count ≥ **116** (today's count; confirm via `ctest -N` before starting).
3. `bash .claude/hooks/pluginval_gate.sh` green on Windows (behavior unchanged).
4. Theme.h mono font platform-selected; Windows still renders Consolas (unchanged).
5. CMakeLists mac branch present and provably inert on Windows: configure output unchanged (deploy ENABLED as today), Windows artefacts unchanged.
6. The Win32 call-site enumeration below is recorded in this spec — planner-verified complete 2026-07-07.

*Phase 2 (precondition: Joe's private GitHub repo pushed — Amendment A1; item 7 is the mandatory FIRST work item, per ROUTING Rule 2):*
7. Verification surface stood up, BOTH layers: (a) hooks OS-branch as spec'd in the file plan — `build_test.sh`/`pluginval_gate.sh` branch on `uname`, agent.config untouched (Rule 10); (b) `.github/workflows/macos-build.yml` on macos-14 (arm64): checkout → `bash .claude/hooks/build_test.sh` → provision the official mac pluginval release binary → `bash .claude/hooks/pluginval_gate.sh` (the hook's auto-skip must NEVER fire in CI — the workflow asserts pluginval is present before invoking; a skipped secondary gate in CI = a FAILED run) → AU machine gate per criterion 11 (install the .component to `~/Library/Audio/Plug-Ins/Components`, `killall -9 AudioComponentRegistrar || true`, `auval -v aumu Gsmp Jtgt` — from the FIRST run, Amendment A2) → assert `lipo -archs` reports `x86_64 arm64` on the VST3 plugin binary, the AU plugin binary, AND the bundled ORT dylib → ad-hoc re-sign the finished bundle(s) AFTER the ORT dylib is inserted (`codesign --force --deep --sign -`) → package bundles with `ditto -c -k --keepParent` BEFORE `actions/upload-artifact` (upload-artifact destroys symlinks and exec bits; a bare-uploaded .vst3 arrives broken and mimics a Gatekeeper failure — never upload an unzipped bundle tree) → upload the ditto-zips as the artifact. Triggers: `workflow_dispatch` plus a narrow push filter chosen by the implementer — NEVER every push to master (mac minutes bill at 10×, Risk R10). **All 116 ctest cases pass on the mac runner.**
8. Universal2 VST3 AND AU bundles build in the same CI run — `lipo -archs` on both plugin binaries AND the bundled ORT dylib all report `x86_64 arm64`.
9. Mac pluginval strictness 5 IN CI, zero failures, including "Open plugin (cold)" (no synchronous heavy work on the construct path on mac either). The arm64 runner image ships Rosetta 2: best-effort, also run the x86_64 pluginval slice under Rosetta; if infeasible, the second arch is build-only coverage — record the gap explicitly in the CI log and this spec, never silently.
10. Dylib-absent degradation parity, split machine/human: MACHINE half in CI — the workflow copies the built bundle, strips the ORT dylib, re-ad-hoc-signs, and pluginval still passes (plugin loads, no crash). HUMAN half on MACOS_TEST_CHECKLIST.md — separation/transcription report engine-unavailable gracefully (mirrors Windows behavior with missing DLLs).
11. AU machine gate — RUNS FROM THE FIRST GREEN CI RUN (Amendment A2 dissolved the VST3-first staging: with Logic as the only hands-on host, the AU bus-layout risk R4 sits on the critical path to ALL human validation and is de-risked FIRST, not last): the AU bundle builds alongside VST3 in every CI run; the workflow installs the .component to `~/Library/Audio/Plug-Ins/Components` on the runner, `killall -9 AudioComponentRegistrar || true`, then `auval -v aumu Gsmp Jtgt` passes with the 16 optional output buses declared. If auval rejects the bus layout: STOP, escalate to planner with the auval output — do not silently reduce buses. A VST3-pluginval PASS may be checkpointed while an auval escalation is open, but NO artifact ships to the friend until auval is green.
12. LOGIC PRO (AU) hands-on — FRIEND-EXECUTED, JOE-DIRECTED over Zoom, following MACOS_TEST_CHECKLIST.md row by row (Amendments A1+A2; Logic loads AU ONLY — the .component is the artifact under test): insert GentSampler on a Software Instrument track (aumu; if it does not appear after install, run the checklist's rescan step first — restart Logic, then Plug-in Manager full rescan); pads driven via MIDI controller or Logic's Musical Typing (Cmd-K); drop track = drag the audio file FROM FINDER into the plugin editor (drag from Logic's browser/arrange = separate BONUS row — different drag source; report behavior, don't debug live); BPM/key detection, cue points, play/flip on pads, one CPU separation (6 stems), one kit save/load. MULTI-OUT row: re-insert choosing the multi-output variant from Logic's instrument picker and add aux channels via the mixer "+" — if no multi-output variant is offered despite auval green, that is a FINDING (report, stop). STATE row: save the Logic project, close, reopen, verify full state restore (the AU state path's first real-host exercise). Named visual smoke items: arc-knob glow (glowSprite), hero waveform paint, mono numeric readouts (Menlo), at 1040×700 and the 880×592 floor — judged from NATIVE screenshots the friend captures (Cmd-Shift-4) and sends, never from Zoom's compressed stream. OPTIONAL bonus rows (flag, never require): Logic opened under Rosetta (Get Info → "Open using Rosetta") to exercise the x86_64 slice hands-on (partially closes R8/R13); FL Studio mac trial for a VST3 smoke (R14). Each row records PASS/FAIL; **Joe remains the sole PASS arbiter** — the friend is the hands, Joe is the judgment (loop-4 human touchpoint unchanged).
13. First-run model download (~1.79 GB) completes ON THE FRIEND'S MAC via the JUCE network stack (checklist step — his bandwidth and disk; pre-flight requires ≥ 5 GB free), lands in the JUCE-resolved `~/Documents/GentSampler/models`, SHA-256 verified (ModelDownloader is pure-JUCE — planner-verified — but must be observed working; observation = friend reads the completion state to Joe over Zoom).
14. CPU separation output sanity vs Windows: same 6 stems for the same input, no audible discrepancy (Joe spot-check; bit-exactness not required). Relay path (Amendment A2): the friend sends the exported stem WAVs, or plays them with Zoom's "Original sound for musicians" enabled — never judged over default Zoom audio compression.
15. Transfer-quarantine handling (Amendment A1 — the original "local builds load unsigned" premise is FALSE under the remote-tester model): every bundle reaching the friend's Mac is TRANSFERRED (download/AirDrop/file send) and carries the `com.apple.quarantine` xattr — Gatekeeper WILL block an unsigned/ad-hoc bundle. The private-testing recipe, both halves mandatory: (a) CI ad-hoc re-sign (criterion 7 — arm64 refuses to run wholly unsigned code at all); (b) MACOS_TEST_CHECKLIST.md includes, as a mandatory post-install step on the friend's machine, `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/GentSampler.component` (primary — Logic loads AU only; the `.vst3` analog only if the optional VST3 bonus row is run). This recipe is acceptable for PRIVATE testing ONLY and never appears in public-facing documentation. **Distribution signing + notarization remains a SHIP GATE recorded in BACKLOG "BEFORE PUBLIC RELEASE" (mirrors the Windows cert gate), executed if/when Joe enrolls in the Apple Developer Program — it does NOT block this port's close.**

*Win32 call-site enumeration (criterion 6 content — planner-verified complete 2026-07-07):*
- StemSeparator.cpp 25-27 (windows.h), 62-181 (ensureOrtLoaded: GetModuleHandleExW / GetModuleFileNameW / LoadLibraryExW / GetProcAddress / kCudaDlls / wchar_t), 320-324 + 371-375 (wide-path session ctor, already branched) → file plan (a)-(d).
- Transcriber.cpp 91-97 — already branched, portable.
- Theme.h 72 — Windows-only font (not an API; port-relevant) → file plan.
- Theme.h 323-342 — leak pattern, portable, keep.
- CMakeLists.txt — win-x64-gpu ORT URL; MSVC `/utf-8` (generator-expression-guarded, fine); POST_BUILD .dll copies; `C:/` deploy step → file plan.
- Hooks — taskkill, VS generator, `%LOCALAPPDATA%`, pluginval.exe → file plan.
- EngineMath.h normPathForCompare/pathIsWithin — separator-agnostic, portable; case-insensitive compare matches default APFS (case-sensitive volumes = accepted edge, see Risks).
- ModelDownloader.cpp/.h — pure JUCE URL/WebInputStream; the WinINet reference is a comment about JUCE's backend, not a call site. Portable.
- No other hits in Source/ for LoadLibrary / GetProcAddress / __declspec / CreateFile / SHGet / WideCharToMultiByte / _WIN32 / wchar_t / backslash path literals (planner grep 2026-07-07).

**UI acceptance criteria** — omit: no new UI is being built; the existing JUCE editor must simply render/operate identically on mac (covered by criterion 12's Logic Pro (AU) hands-on pass, not a separate UI gate). If the mac editor shows any visual regression (font rendering, DPI scaling, CoreGraphics-specific paint artifacts), Joe's manual pass is the gate — flag, don't guess at CoreGraphics parity from Windows-side reading.

**Verification commands**

- Phase 1 (Windows, unchanged): `bash .claude/hooks/build_test.sh` (primary) + `bash .claude/hooks/pluginval_gate.sh` (secondary).
- Phase 2 (mac, via CI): the SAME two hook invocations, executed BY the macos-14 runner inside `.github/workflows/macos-build.yml` (Rule 10; agent.config unchanged; the workflow is a thin wrapper, never a second copy of gate content). Their required mac content: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` + `cmake --build build --parallel` + `ctest --test-dir build --output-on-failure` (single-config generator), then mac pluginval strictness 5 on the .vst3 AND `auval -v aumu Gsmp Jtgt` on the .component — BOTH from the FIRST CI run (Amendment A2) — plus the CI `lipo -archs` asserts. Final arbiter: the friend-executed, Joe-directed MACOS_TEST_CHECKLIST.md pass IN LOGIC PRO over Zoom (criterion 12) — Joe judges, the friend operates.

**Out of scope**

- AAX / Pro Tools support (explicitly excluded per the task brief).
- Mac installer/notarization packaging tooling (pkgbuild, productbuild, DMG, or equivalent) — propose as its own follow-on spec once Phase 2 produces a working signed bundle, mirroring how INSTALLER_SPEC.md followed PACKAGING_SPEC.md on Windows.
- JUCE commercial license purchase — a shared gate across Windows and Mac public release, already tracked in BACKLOG.md "BEFORE PUBLIC RELEASE"; do not duplicate or block this port on it (private/dev builds are unaffected).
- CUDA/GPU work of any kind — mac GPU story is CoreML (noted as a future pivot in GPU_HANDOFF.md §9), explicitly not part of this port; CPU-only mac separation is the v1 target, matching the Windows shipping path.
- Any ONNX Runtime version change — stays pinned at 1.18.1. **RESOLVED as unnecessary:** ORT 1.18.1 ships official macOS release assets (see Settled facts); no divergence from the pin, ever, on any platform.
- Windows-side installer (`installer/GentSampler.iss`, `installer/make_installer.bat`, `build.bat`) — untouched; no changes to the Windows release/install pipeline.
- Rewriting/refactoring the classifier, granular engine, kit system, or any other v1-complete feature beyond what's needed to make it compile/run cross-platform (no scope creep into feature work).

**Tier assignment**

- StemSeparator.cpp dlopen/dlsym refactor (Phase 1, item 1) — **IMPLEMENTER**. Judgment work: matching Windows semantics (LOAD_WITH_ALTERED_SEARCH_PATH equivalent via `dladdr` + absolute-path dlopen, symbol resolution, error handling) in a different OS ABI; a machine gate (compile + existing ctest) catches syntax/logic breakage but not architectural correctness.
- CMakeLists.txt mac-branch scaffolding (Phase 1, item 5) — **IMPLEMENTER**. Structural/architectural; CMake correctness across platforms is not BULK-safe given the landmine history around COPY_PLUGIN_AFTER_BUILD and the deploy step.
- Win32 call-site enumeration — COMPLETE (drafter-produced, planner-verified by independent grep 2026-07-07; two additions: the fail-open non-Windows stub and Theme.h:72 Consolas).
- Phase 2 execution — **IMPLEMENTER on the Windows box, driving CI** (JOE-DECISION 4 RESOLVED, Amendment A1): all code, workflow, and gate iteration happens locally and on GitHub Actions. NO agent ever runs on the friend's machine, and the friend never receives repo access or source — build artifacts only, relayed by Joe. Not BULK.
- No BULK-eligible work identified in Phase 1. If the planner identifies a pure mechanical sub-task later, name the machine check explicitly at that time.

**Loop budget** — Default per `.claude/agent.config` (MAX_IMPL_ATTEMPTS=3, MAX_REVIEW_CYCLES=2), **except the `ensureOrtLoaded()` refactor task: MAX_IMPL_ATTEMPTS=2** (planner-accepted: it is the epicenter of two prior landmines — the 2026-07-04 cold-open hang and the load-bearing manual-load design — and its gate catches regressions fast; two failures there mean the design needs the planner, not a third swing).

**Checkpoints**

1. (Phase 1) `ensureOrtLoaded()` refactor + Theme.h font branch land; Windows build/ctest/pluginval green — commit.
2. (Phase 1) CMakeLists mac branch + enumeration recorded; Windows configure/build provably unchanged — commit. **Phase 1 close; CLAUDE.md updated.**
3. (Phase 2 — after Joe's push) Verification surface: hooks OS-branch + `.github/workflows/macos-build.yml` land; FIRST GREEN CI RUN (mac configure + build BOTH bundles + all 116 ctest) — commit.
4. (Phase 2) CI machine gates: pluginval PASS (VST3, universal2) AND auval PASS (AU), lipo asserts green — commit. (An auval bus-layout failure = immediate planner escalation per criterion 11; the pluginval half may be committed while that escalation is open, but nothing ships to the friend until auval is green.)
5. (Phase 2) MACOS_TEST_CHECKLIST.md authored (Logic/AU per criterion 12) + first artifact relayed to the friend (.component primary; .vst3 included for completeness) — commit.
6. (Phase 2) Friend/Zoom LOGIC validation pass GREEN, Joe-arbitrated — task close; CLAUDE.md updated.

**Settled facts (planner, 2026-07-07)**

- ORT 1.18.1 SHIPS official macOS release assets (verified against microsoft/onnxruntime v1.18.1 release assets via the GitHub API): `onnxruntime-osx-universal2-1.18.1.tgz` (16.3 MB), `onnxruntime-osx-arm64-1.18.1.tgz` (7.7 MB), `onnxruntime-osx-x86_64-1.18.1.tgz` (8.7 MB). All CPU-only — which matches the CPU-only mac target exactly. Resolves draft OQ2/OQ3: no version divergence from the pin; universal2 makes a single universal bundle the REQUIREMENT (criterion 8), not a stretch goal.
- dlopen strategy (was OQ4) — RULED: dlopen/dlsym mirror of the Windows manual-load pattern (`dladdr` → absolute-path `dlopen` → `dlsym("OrtGetApiBase")`), dylib bundled inside the plugin bundle. The zero-load-time-dependency constraint survives on mac not for CUDA reasons (there is no CUDA pack) but for graceful degradation parity (criterion 10) and one architecture across platforms. Direct-link rejected: a load command on a missing dylib fails the whole plugin load, and it buys nothing the 16 MB bundled dylib doesn't already provide.
- AU timing (was OQ9/OQ10) — RULED, then PARTIALLY SUPERSEDED BY AMENDMENT A2: AU is in Phase 2 v1 with `auval -v aumu Gsmp Jtgt` as its machine gate (unchanged), but the "staged strictly after VST3 goes pluginval-green" ordering is DISSOLVED — the friend's DAW is Logic (AU-only), so AU builds and auval runs from the FIRST CI run (criterion 11), front-loading the bus-layout risk that now gates all hands-on validation. JUCE derives `AU_MAIN_TYPE` from `IS_SYNTH TRUE` (the draft's "new flags" claim was overstated). The 16-optional-bus AU risk is real but speculative — resolved by auval, with a pre-wired STOP-and-escalate (criterion 11), never a silent bus reduction.
- Mac verification surface (was OQ6) — RULED: OS-branch inside the EXISTING hooks, `agent.config` untouched (Rule 10). Phase 2's mandatory first work item (criterion 7).
- CoreGraphics landmine re-audit (was OQ5) — confirmed as drafted, upgraded to named smoke items in criterion 12 + Risk R7. The glowSprite leak stays on all platforms.

**Risks**

- R1 — The pre-existing `#else return true;` fail-open stub in `ensureOrtLoaded()`: on mac it reports ORT ready with a null API table → host crash on first `Ort::` call. Eliminated by criterion 1; the single most dangerous latent defect in this port.
- R2 — Hardened runtime / library validation at distribution time: a dlopen'd dylib must be signed as part of the bundle for notarization; re-sign the ORT dylib with our identity during bundle signing (standard practice). Deferred to the ship gate but recorded so the dlopen design isn't blindsided later.
- R3 — ORT osx archive layout may differ from the win zip (likely no `providers_shared` dylib; symlinked dylib names). The `file(GLOB_RECURSE)` header locate is robust; implementer verifies lib/ contents at first mac configure.
- R4 — AU 16-bus layout may fail auval (JUCE AU dynamic-bus history). Gate = auval; escalation pre-wired (criterion 11).
- R5 — Font/text-metric drift on mac beyond the Menlo fix; the Windows capture rig doesn't exist on mac — Joe's eyeball pass (criterion 12) is the gate.
- R6 — Case-sensitive APFS volumes weaken the case-insensitive `pathIsWithin` self-drop guard (false negative only). Accepted edge; no action.
- R7 — CoreGraphics off-screen `juce::Image` behavior (analog of the Windows D2D no-op landmine): glowSprite renders off-screen at first call — named smoke item in criterion 12.
- R8 — If Joe's Mac is Apple Silicon, the x86_64 slice is under-tested (Rosetta pluginval best-effort, criterion 9).
- R9 — Logic Pro AU hosting differences (Amendment A2): AU rescan/cache after install (AudioComponentRegistrar), multi-output requires the multi-out variant at insert + manual aux creation in the mixer, drag-drop into the editor differs by source (Finder vs Logic browser/arrange), Musical Typing as the no-controller MIDI path, AU state save/reopen inside a Logic project — covered only by the criterion-12 checklist pass. Logic re-runs auval at scan: CI auval green is the leading indicator the plugin appears in Logic at all.
- R10 — GitHub Actions macOS minutes bill at 10× (free tier ≈ 2,000 raw ≈ 200 effective mac-minutes/month; a universal2 JUCE+ORT build plausibly costs 15-25 min/run ≈ 8-12 runs/month). Mitigation: workflow_dispatch trigger discipline (criterion 7), caching if cheap, or Joe upgrades the plan. Exhaustion here escalates as a spec-sizing problem per Rule 5, never as silent gate-skipping.
- R11 — `actions/upload-artifact` destroys symlinks and exec permissions; a bare-uploaded .vst3/.component arrives broken on the friend's Mac and MIMICS a Gatekeeper/codesign failure. Rule (encoded in criterion 7, recorded here so nobody "simplifies" it away): `ditto -c -k --keepParent` zip BEFORE upload, always.
- R12 — Remote-loop limits (accepted, Amendment A1): no debugger on the failure machine; diagnosis = crash logs the friend exports (`~/Library/Logs/DiagnosticReports`, .ips), CI pluginval/auval output, and added logging; loop latency = CI time + friend availability; Zoom compression hides subtle visual defects (mitigated by native-screenshot rule, criterion 12). If Phase 2 exhausts its budgets on a defect undiagnosable remotely, the arbitrated fallback is cheap local hardware (e.g. used M1 Mac mini) — an escalation outcome, not the plan.
- R13 — Friend's Mac architecture UNKNOWN (probably Apple Silicon, unconfirmed). If Intel: the arm64 slice becomes the under-tested one (inverts R8) and Rosetta-related items change meaning. Pre-flight (checklist + Joe-ask): `uname -m`, macOS version (must satisfy BOTH ≥ 11.0 — our deployment target — AND his Logic version's own requirement; any Mac running current Logic Pro clears 11.0 automatically), free disk, Logic Pro version (Amendment A2 — confirm Logic Pro, NOT GarageBand: GarageBand hosts AU but has no multi-output and would silently shrink criterion 12).
- R14 — Mac VST3 ships with MACHINE-ONLY coverage (Amendment A2, accepted): pluginval strictness 5 in CI is its only gate — no VST3-hosting mac DAW is available for hands-on. Accepted for port close: Joe's Windows FL validation covers the plugin logic, pluginval exercises the VST3 wrapper lifecycle; the untested delta is real-host mac-VST3 quirks only. Recorded, never silent — README's mac status (when written) must say "VST3 built + pluginval-validated; hands-on validated in Logic Pro (AU)". Optional mitigation = the FL-mac-trial bonus row in criterion 12; flagged, never required.

**JOE-DECISIONS — status as of 2026-07-07 approval + Amendment A1**

1. **Mac hardware — RESOLVED 2026-07-07:** neither original branch. No local Mac; validation runs on a remote friend's Mac via Zoom (architecture unconfirmed — see R13). Build machine = GitHub Actions CI (Amendment A1); Phase 1 executes NOW; Phase 2 gated on Joe's private-GitHub push. The friend's machine is never a build machine and never receives source.
2. **Apple Developer Program enrollment — RESOLVED 2026-07-07:** deferred to the ship gate per planner recommendation (BACKLOG "BEFORE PUBLIC RELEASE"). Private testing uses the criterion-15 ad-hoc + quarantine-strip recipe.
3. **JUCE commercial purchase — OPEN** (shared public-release gate, unchanged; BACKLOG "BEFORE PUBLIC RELEASE"); confirm at purchase that the license covers mac builds (expected: one license, all platforms — verify, don't assume). Flag, don't block.
4. **Phase 2 execution mode — RESOLVED 2026-07-07 (Amendment A1, planner-advised):** implementer on the Windows box driving CI; friend executes MACOS_TEST_CHECKLIST.md under Joe's Zoom direction; artifacts relayed by Joe; no agent and no source on the friend's machine.
5. **Hands-on host — RESOLVED 2026-07-07 (Amendment A2):** the friend's DAW is Logic Pro, which loads AU ONLY — Logic/AU IS the hands-on validation host (criterion 12), not a bonus. auval (in CI) remains the AU machine gate and now runs from the FIRST CI run. The former FL-for-Mac hands-on is inverted into the optional bonus (VST3 smoke via FL mac trial — flag, never require; R14).

---

**Provenance:** drafted by spec-drafter 2026-07-07 (audit files: CLAUDE.md, ROUTING.md, BACKLOG.md, GPU_HANDOFF.md, Source/StemSeparator.cpp, Source/Theme.h, Source/Transcriber.cpp, Source/ModelDownloader.h/.cpp, Source/PluginProcessor.cpp, Source/EngineMath.h, CMakeLists.txt, tests/PathGuardTests.cpp, .claude/hooks/*, .claude/agent.config, installer/GentSampler.iss, README.md). Planner-reviewed, corrected, and owned 2026-07-07: independent Win32-completeness grep; two audit additions (fail-open stub, Consolas); rulings on dlopen strategy, AU staging, verification surface, loop budget; ORT mac-binary availability lead-verified via the GitHub API.

---

## AMENDMENT A1 — Remote-friend validation model + CI build machine (planner, 2026-07-07)

Triggered by Joe's approval-gate answers: spec APPROVED; no local Mac — a remote friend
tests on his own Mac via Zoom (arch unconfirmed); Apple enrollment deferred to ship gate;
Phase-2 mode delegated to the planner. Lead-verified facts: the repo has NO git remote;
this Windows box cannot produce mac binaries.

**Ruling 1 — Build machine is GitHub Actions CI, not the friend's Mac.** A macos-14
(arm64) runner builds universal2 via `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`, runs
ctest natively, runs mac pluginval and (once AU exists) auval, and uploads ditto-zipped
bundle artifacts. The friend's Mac as build machine is rejected: source on uncontrolled
hardware, Zoom-driven toolchain installs, iteration coupled to a third party's calendar.
NEW PREREQUISITE: Joe creates a private GitHub repo and pushes; Phase 2 cannot start
without it. Rule 10 holds — the workflow is a thin wrapper invoking the SAME hooks;
the hooks-OS-branch item survives and criterion 7 now covers both layers.

**Ruling 2 — Phase 1 executes NOW.** The branch-(b) parking clause existed because mac
code would have had no compiler; CI IS the compiler, and it back-checks Phase-1 code at
first push. Condition dissolved.

**Ruling 3 — Transfer quarantine.** Criterion 15's "local builds load unsigned" premise
is false here: transferred bundles carry `com.apple.quarantine` and Gatekeeper blocks
unsigned/ad-hoc code. Private-testing recipe = CI ad-hoc re-sign + documented
`xattr -dr com.apple.quarantine` strip on the friend's machine (checklist step),
PRIVATE testing only; ship gate unchanged. MACOS_TEST_CHECKLIST.md is a new deliverable
(precedent: INSTALLER_TEST_CHECKLIST.md / CLEAN_MACHINE_CHECKLIST.md).

**Ruling 4 — Operating model (JOE-DECISION 4).** Build: CI on demand (workflow_dispatch;
mac minutes bill at 10×, R10). Machine gates: entirely in CI — the friend is never part
of machine verification. Validation: friend + Zoom + checklist; native screenshots for
visual judgment; Joe is the sole PASS arbiter. Iteration: finding → fix on Windows →
push → CI → Joe relays the new artifact → friend re-tests only failed rows. Accepted
limits (R12): no debugger on the failure machine (crash-log export instead), CI+friend
latency per loop, Zoom compression. Security boundary: no agent on the friend's machine,
no repo access for the friend — Joe downloads artifacts and sends the zip.

**Unchanged by this amendment:** AAX stays out; ORT stays pinned 1.18.1 (universal2
osx asset); AU staging order; loop budgets (including the ensureOrtLoaded 2-attempt cap);
all Phase-1 technical content; the ship-gate structure.

---

## AMENDMENT A2 — Logic Pro is the hands-on host; AU staging dissolved (planner, 2026-07-07)

Triggered by Joe's asks follow-up: the friend's DAW is **Logic Pro**, which hosts AU
ONLY — no VST3 support. Friend availability is arranged (A1 ask 3 done); machine info
(`uname -m`, macOS version, disk) still pending (R13). Phase 1, already executing, is
UNAFFECTED (the AU format was in the Phase-1 CMake plan; Windows inertness, criterion 5,
unchanged).

**Ruling 1 — AU staging dissolved; both machine gates from the first CI run.** The
VST3-first ordering existed to avoid debugging two formats at once on scarce local
hardware; in CI both gates are one job. Criterion 11's auval STOP-and-escalate (16-bus
layout, R4) now gates ALL human validation — front-loaded to the first green run so a
bus-layout rejection surfaces on day one of Phase 2, not after VST3 closes. VST3
pluginval PASS may checkpoint while an auval escalation is open; nothing ships to the
friend until auval is green.

**Ruling 2 — Criterion 12 rewritten for Logic**, same smoke content plus Logic ceremony:
AU rescan (Logic re-runs auval at scan — CI auval green predicts visibility), aumu
Software Instrument insert, Musical Typing fallback, Finder-drag canonical (Logic-browser
drag = bonus row), multi-output insert-variant + mixer-aux row (variant missing despite
auval green = FINDING), and one NEW row: Logic-project save/close/reopen — the AU state
path's first real-host exercise. Optional bonus rows: Rosetta-Logic (hands-on x86_64,
partially closes R8/R13) and FL mac trial (VST3 smoke).

**Ruling 3 — Mac VST3 accepted at machine-only coverage (R14).** No VST3-hosting mac DAW
is available; pluginval strictness 5 is its gate. Windows FL validation covers plugin
logic; the delta is mac-VST3 host quirks. Recorded explicitly (README nuance required);
FL-trial bonus row is the optional mitigation, never a requirement of port close.

**Ruling 4 — Checklist/artifact deltas.** The .component is the primary artifact and the
primary quarantine-strip target; pre-flight swaps FL-trial for Logic Pro version and
explicitly excludes GarageBand (no multi-output — would silently shrink criterion 12);
separation spot-check audio relayed as exported WAVs or Zoom "Original sound", never
default Zoom compression. Crash-log guidance unchanged.

**Unchanged by this amendment:** AAX out; ORT pinned 1.18.1; CI build machine + security
boundary (A1); quarantine recipe (path target updated only); loop budgets; all Phase-1
technical content; the ship-gate structure; Joe as sole PASS arbiter.
