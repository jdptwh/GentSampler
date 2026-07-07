# INSTALLER_SPEC — FINAL (planner-owned; **Joe-APPROVED 2026-07-07** — incl. Inno Setup 6 tooling authorization)

## APPROVAL RECORD (Joe, 2026-07-07)
- Spec approved as written; **Inno Setup 6 authorized** as a one-time authoring-machine tool.
- Scope Q&A at approval: installer + plugin are WINDOWS-ONLY; the VST3 installs to the
  standard Common Files path scanned by all Windows VST3 DAWs (hands-on validated in FL;
  pluginval strictness 5 = host-agnostic evidence for others; Pro Tools/AAX not supported).
  macOS port = separate future project, logged in BACKLOG at Joe's request.

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ master tip (post-PACKAGING close).
**Provenance:** drafted by SPEC-DRAFTER; reviewed, corrected, and OWNED by the PLANNER 2026-07-07. Sources verified: BACKLOG.md INSTALLER item, PACKAGING_SPEC.md (P3 ledger + Joe close-out), build.bat, CMakeLists.txt:1–160, CLEAN_MACHINE_CHECKLIST.md, Source/ModelDownloader.cpp, Source/PluginProcessor.cpp:933–940/1322–1352 (models-dir resolution — install-independent, claim TRUE), THIRD_PARTY_LICENSES/README.md:23–47 (JUCE commercial declaration), .claude/agent.config, .gitignore (NO dist/ entry today), README.md.

**HEADLINE DECISION FOR JOE AT APPROVAL:** this spec adds **Inno Setup 6** as a one-time authoring-machine tool. Not a repo, build, or runtime dependency — the committed `.iss` is plain text; the compiler runs only when `make_installer.bat` is invoked by hand. Joe authorizes, or the spec re-scopes to the zip+install.bat fallback (returns to the planner; do not improvise).

**Objective** — Produce a double-clickable, unsigned Windows installer (`GentSamplerSetup-<version>.exe`) installing the CUDA-free VST3 payload to `C:\Program Files\Common Files\VST3` with correct elevation and a clean uninstall entry, built from existing Release artefacts with ZERO changes to CMakeLists.txt or build.bat; proven locally on the dev box (T4), then end-to-end on Joe's separate machine with no toolchain (T5 — closing the clean-machine validation Joe deferred to "the installer-package test", PACKAGING P3 Record). Code-signing and the JUCE commercial purchase are documented as before-public-release gates, not performed.

---

## Planner rulings (the seven open questions + the local-test gate)

- **OQ-INST-A (Standalone in payload) — RULING: VST3-only. No Components toggle in v1.** "Shipped" = runs-in-FL (CLAUDE.md); a Standalone component doubles payload surface and test matrix for zero distribution value. Standalone stays a build.bat dev artefact. Revisit only if Joe asks at approval.
- **OQ-INST-B (tooling) — RULING: Inno Setup 6, Joe authorizes at approval.** Free incl. commercial use, committable text `.iss`, scriptable fail-closed compile (ISCC), native `PrivilegesRequired=admin` elevation, automatic Add/Remove entry + uninstaller — all four of which zip+install.bat lacks. If Joe declines, spec returns to the planner for a zip+bat re-scope.
- **OQ-INST-3 (version sync) — RULING: PARSE, never hand-copy.** `make_installer.bat` extracts the version from `project(GentSampler VERSION x.y.z)` (CMakeLists.txt line 2 — the single source of truth PACKAGING P5 established) and passes it via `/DAppVer=...`; the `.iss` carries `#ifndef AppVer` → `#error` so a bare ISCC compile fails loudly instead of embedding a stale number. Output filename derives from the same parse.
- **OQ-INST-4 (dist/ gitignored) — RULING: yes, and it requires an edit the draft omitted.** Verified: `.gitignore` has no `dist/` entry today (the incidental `*.exe` rule must not be relied on). T3 adds it; `.gitignore` joins the file plan.
- **OQ-INST-5 (PACKAGING P3 ledger ownership) — RULING: THIS spec's Record owns the evidence; PACKAGING_SPEC.md gets one pointer line, no duplication.** T5's row holds Joe's full report; T6 appends one sentence to PACKAGING_SPEC.md's close-out: "Clean-machine run completed via the installer-package test — evidence in INSTALLER_SPEC.md Record (T5)." Scope precisely: the installer test closes the **no-toolchain clean-machine + Joe FL round-trip + first-run model download** rows; it does NOT close the `IS_DIRECTORY` negative branch (stands on WAVE4 F4's `cmake -P` proof) nor build.bat's non-admin fallback. No overclaim.
- **OQ-INST-6 (UAC evidence) — RULING: Joe's written report + one pasted artifact; no screenshot requirement.** The UAC secure desktop can't be captured by normal tooling. The meaningful invariant — an elevated write to Common Files succeeded — is proven by the artifact we CAN get: Joe pastes/photographs the `dir` of the installed bundle, which doubles as the CUDA-free check. Checklist report-back: UAC seen y/n, errors verbatim, the dir listing, literal path confirms `Program Files` NOT `Program Files (x86)`.
- **OQ-INST-7 (script location) — RULING: `installer\make_installer.bat`, co-located with the `.iss`.** No `scripts/` dir exists and one file doesn't justify one; repo root is the user-facing surface. Output lands in repo-root `dist\` (gitignored).
- **LOCAL-TEST GATE — RULING: no P2-style Joe-approval dry-run for T4; a stated protocol suffices, because everything the installer touches is machine-regenerable** (binary + 2 ORT DLLs + license files = one build.bat run away). The real hazard is the ACL-grant loss on uninstall — hence T4's mandatory restore steps. Joe approving THIS SPEC constitutes informed consent for the dev-box test.

**Planner corrections to the draft:** (1) "license display" page removed — no EULA exists and JUCE mode is commercial-declared/purchase-pending; inventing legal text is forbidden (PACKAGING OQ-PKG-C precedent). License TEXTS ship as installed files; an EULA decision joins T6's gates. (2) Added T4 local dev-box install/uninstall test — the draft shipped an untested exe straight to Joe's box. (3) Added the 64-bit directives AC — without `ArchitecturesInstallIn64BitMode`, Inno resolves `{commoncf}` to `Program Files (x86)` and FL never finds the plugin. (4) Stable-AppId AC (no stacked Add/Remove entries). (5) Hardcoded "1.1.0" removed everywhere per OQ-INST-3. (6) `.gitignore` added to the file plan. (7) SmartScreen/Mark-of-the-Web guidance added to Joe's checklist. (8) `Documents\GentSampler` non-deletion made an explicit AC.

---

## File plan

| Path | Task |
|---|---|
| `installer\GentSampler.iss` (new) | T2 |
| `installer\make_installer.bat` (new) | T3 |
| `.gitignore` (add `dist/` line only) | T3 |
| `INSTALLER_TEST_CHECKLIST.md` (new, repo root) | T5 |
| `README.md` ("Install (no build)" section, only after T5 passes) | T6 |
| `PACKAGING_SPEC.md` (ONE pointer sentence per OQ-INST-5) | T6 |
| `BACKLOG.md` (INSTALLER item resolved; before-public-release items remain) | T6 |
| `CLAUDE.md` ("Current state" update) | T6 |
| `INSTALLER_SPEC.md` (Record) | all |
| `dist\` (created, gitignored, never committed) | T3 |

**CMakeLists.txt and build.bat: UNTOUCHED.** Any other file = reviewer FAIL.

---

## T1 — Payload definition + pre-flight verification rule (IMPLEMENTER)

- **Payload = the VST3 bundle** staged from `build\GentSampler_artefacts\Release\VST3\GentSampler.vst3\` (junction to D: — ISCC follows it): `Contents\x86_64-win\{GentSampler.vst3, onnxruntime.dll, onnxruntime_providers_shared.dll}` — exactly the 3 keepers PACKAGING proved. Plus licenses installed into the bundle: `THIRD_PARTY_LICENSES\` tree + repo `README.md` → `...\Contents\Resources\`. No models, no CUDA names, no Standalone.
- **Pre-flight allowlist check** (in make_installer.bat before ISCC): scan the staged dir for `cublas*.dll cudart*.dll cudnn*.dll cufft*.dll curand*.dll nvrtc*.dll onnxruntime_providers_cuda.dll` — any match = exit non-zero, no compile. Also fail if any keeper is absent.
- The installer never references `Documents\GentSampler` in any section (models resolve via `userDocumentsDirectory`, PluginProcessor.cpp:936/:1325 — install-independent).

**AC-T1:** (1) payload table recorded with live `dir` of the staged artefact pasted; (2) the 7-pattern + 3-keeper check written into T3's script verbatim; (3) models-dir independence noted with the two source-line citations.

## T2 — `installer\GentSampler.iss` authoring (IMPLEMENTER)

The `.iss` MUST satisfy:
1. `#ifndef AppVer` → `#error` guard; `AppVersion={#AppVer}`; `OutputBaseFilename=GentSamplerSetup-{#AppVer}`.
2. `PrivilegesRequired=admin`; **`ArchitecturesAllowed=x64compatible` + `ArchitecturesInstallIn64BitMode=x64compatible`** (Inno 6.3+ names; `x64` if older) — else `{commoncf}` is the x86 tree.
3. Fixed destination `{commoncf}\VST3\GentSampler.vst3\...` mirroring the bundle; **no directory page** (`DisableDirPage=yes`) — the VST3 location is standard-mandated.
4. Stable `AppId={{...}}` GUID, generated once and committed.
5. **No `LicenseFile` page** (no EULA exists — correction 1). Licenses install as files per T1.
6. **No `[UninstallDelete]`, `[Registry]`, or `[Run]` sections touching anything outside `{commoncf}\VST3\GentSampler.vst3`.** Uninstall removes only installed files + then-empty dirs. `Documents\GentSampler` never referenced.
7. In-use files (FL open): accept Inno's default abort/retry; **no restart-manager force-close, no `/FORCECLOSEAPPLICATIONS`** — the never-terminate-FL rule applies to the installer too.

**AC-T2:** (1) ISCC compiles clean with `/DAppVer` and **fails with the #error without it**; (2) `[Files]` list = exactly the T1 payload (reviewer diff-reads); (3) all seven requirements visible in the committed file; (4) installed Inno Setup version recorded in the Record (tool-authorization evidence). **Budget: 2 attempts.**

## T3 — `installer\make_installer.bat` + dist/ (IMPLEMENTER)

Flow: parse version from CMakeLists.txt line 2 (`for /f` + `findstr`) → fail if empty → T1 pre-flight (3 keepers present, 7 patterns absent) → locate ISCC (`where ISCC`, fallback `%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe`, fail with install instructions if absent) → compile with `/DAppVer=%VER%` → emit `dist\GentSamplerSetup-%VER%.exe`. **Never triggers a build**; stale/missing artefacts = loud non-zero exit.

**AC-T3:** (1) after a normal green build, one invocation produces `dist\GentSamplerSetup-1.1.0.exe` (current version; filename proves the parse); (2) each failure mode exercised once and recorded — version-parse empty (against a SCRATCH copy of CMakeLists), missing keeper + planted CUDA-pattern dummy (in a scratch STAGING COPY — never plant files in the real artefact dir), ISCC absent (PATH-shadow acceptable) — all exit non-zero with a one-line reason; (3) `dist/` in `.gitignore`, `git status` clean after a run; (4) repo gates green.

## T4 — Local dev-box installer test (IMPLEMENTER executes; protocol non-negotiable; 1 attempt)

Runs BEFORE anything ships to Joe's box. Target = the LIVE deployed folder.
1. FL Studio closed; verify no lock. If locked: report and stop. **No agent ever terminates FL Studio or any user application.**
2. Before-`dir` of the deployed bundle folder.
3. Run the installer (elevated). Verify: 64-bit path (**literal `C:\Program Files\`, not `(x86)`**), dir = the 3 keepers (fresh timestamps) + `Contents\Resources` licenses; Add/Remove entry present with the parsed version.
4. FL smoke: plugin loads, pads sound, `Documents\GentSampler\models` untouched (before/after dir).
5. **Uninstall test:** close FL → uninstall → bundle dir gone, ARP entry gone, `Documents\GentSampler` fully intact.
6. **Restore the dev box (the real hazard):** re-run the installer (or build.bat) → **re-apply the ACL grant via Desktop "Fix GentSampler VST3 Access.bat"** (uninstall deleted the folder carrying the 2026-07-02 grant; an elevated reinstall recreates it WITHOUT the grant, silently breaking the CMake deploy step's non-elevated copy) → `build_test.sh` confirms configure still says `deploy step: ENABLED` and the deploy copy succeeds → `pluginval_gate.sh` green (cold-open timeout = REAL failure, never retried).
7. Any surprise at any step → stop and escalate. No autonomous retry against a system path.

**AC-T4:** all seven steps' evidence (dirs, ARP text/screenshot, configure-log line) in the Record; dev box provably restored (step 6 gates green).

## T5 — Joe's clean-machine test (IMPLEMENTER authors checklist; JOE runs it; no autonomous loop)

`INSTALLER_TEST_CHECKLIST.md` — radically shorter than CLEAN_MACHINE_CHECKLIST.md: **no winget, no git, no CMake, no build**:
1. Copy `GentSamplerSetup-<ver>.exe` over (USB/share). If downloaded, **SmartScreen will warn on an unsigned exe — "More info → Run anyway" is expected** for this private test.
2. Double-click → UAC prompt expected → wizard (**≤5 clicks, no directory choice**) → finish.
3. Verify: paste the `dir` of `C:\Program Files\Common Files\VST3\GentSampler.vst3\Contents\x86_64-win` (3 files, no CUDA names; literal `Program Files`, not `(x86)`).
4. FL: Find more plugins → GentSampler under Installed → Generators → VST3 (no search-path additions — that's the point) → load, drop a track, pads.
5. One CPU separation — **first use downloads ~1.79 GB to `Documents\GentSampler\models`; needs internet; takes a while** — confirm completes.
6. OPTIONAL: uninstall via Add/Remove, confirm plugin disappears from FL after rescan; reinstall if desired.
7. Report back: UAC seen y/n, errors verbatim, the step-3 listing, FL load y/n, separation y/n.

**AC-T5:** (1) checklist self-contained (exe + FL only); (2) Joe's report on record; (3) FL loads + pads + one separation green; (4) this row cited as closure of PACKAGING P3's deferred rows per OQ-INST-5 scope. Failures escalate to the planner with the verbatim report.

## T6 — Documentation + spec-close (IMPLEMENTER; text only)

1. README.md "Install (no build)" section — ONLY after T5 passes; build-from-source path stays. Must state distribution is currently **private** (JUCE commercial purchase pending — do not publish the installer).
2. **"Before public release" list** (spec close + BACKLOG): (a) JUCE commercial license PURCHASE (mode declared 2026-07-07; purchase is the blocker); (b) code-signing certificate (unsigned = SmartScreen friction; Joe business decision); (c) EULA decision (no license text ships today — deliberate).
3. PACKAGING_SPEC.md pointer sentence (OQ-INST-5); BACKLOG INSTALLER item resolved with the three gates surviving; CLAUDE.md updated.

**AC-T6:** diffs text-only; private-distribution warning present; edits match the rulings verbatim; gates green.

---

## UI acceptance criteria
Installer wizard: ≤5 clicks, no directory page, no error dialogs, correct version string, ARP entry present post-install / absent post-uninstall. Verified by T4 (dev box) + T5 (Joe) — no automatable surface (accepted Rule-2 gap, WAVE1 precedent; the plugin editor stays covered by pluginval strictness 5 in T4 step 6).

## Verification commands
Repo-touching checkpoints: `bash .claude/hooks/build_test.sh` + `bash .claude/hooks/pluginval_gate.sh` (agent.config). Explicit manual gates (NOT added to agent.config): ISCC compile (T2), `make_installer.bat` full run + negative tests (T3), the T4 protocol, Joe's T5 report. Reviewer independently re-checks pasted dir evidence (WAVE4 precedent).

## Tier assignment (Rule 1)
T1–T4, T6: IMPLEMENTER (T6 BULK declined — licensing-adjacent prose, PACKAGING P4 precedent). T5 execution: Joe. **No BULK anywhere near Program Files, elevation, or uninstall logic.**

## Loop budget
T1/T3/T6: 3/2 defaults. **T2: 2 attempts.** **T4: 1 attempt** (system path — surprises escalate). **T5: no autonomous loop.** Budget exhaustion = spec-sizing escalation.

## Checkpoints (Rule 9)
**T1 → T2 → T3 → T4 → T5 → T6**, one commit per task (`INSTALLER #N: ...`). T4/T5 checkpoints = Record rows + pasted evidence (little repo diff), PACKAGING P2/P3 pattern. Resume: T1–T3/T6 from git; T4 from Record + live dir state; T5 from Joe's report. Aborted dispatch → diff against last checkpoint first.

## Ground rules (carried forward verbatim, extended to the installer)
- **No agent ever terminates FL Studio or any user application** — and the installer itself must not either (T2 req 7). Install failure while FL holds the plugin = report and stop.
- pluginval cold-open timeout = REAL failure; never retry-loop.
- Program Files discipline: T4's protocol is this spec's authorized exception; hand-deletes outside it still require a Joe-approved dry-run (PACKAGING rule stands).
- `build/` hands-off; T3's negative tests use a scratch COPY of the artefact tree, never the real one.
- No public distribution of any produced exe until the T6 gates clear.

## Out of scope
Code-signing execution; JUCE license purchase; EULA authoring; Standalone packaging; auto-update; macOS/Linux; store distribution; ANY change to CMakeLists.txt, build.bat, or plugin code; `kEnableCuda`/ORT/GPU; the models download mechanism.

## Spec-level acceptance
1. `.iss` + `make_installer.bat` committed, version-parsed, fail-closed, CUDA-pre-flighted (T1–T3).
2. Dev-box install/uninstall/restore cycle green, ACL grant re-applied, deploy step proven still ENABLED (T4).
3. Joe's clean-machine report green (T5) — PACKAGING P3's deferred rows closed per OQ-INST-5 scope.
4. Docs updated with the three before-public-release gates; private-distribution warning present (T6).
5. Reviewer verdict.json PASS, 0 blocking; verdict_lint 0.
6. CLAUDE.md updated at close.

## Risks
- **ACL-grant loss is the sharpest edge:** T4's uninstall deletes the folder carrying the 2026-07-02 grant; without step 6's re-grant + deploy-step re-verify, the next dev build fails mysteriously later. Hence 1-attempt + restore-as-AC.
- **32/64-bit `{commoncf}`:** the most likely silent failure — plugin lands in `(x86)`, FL never sees it, everything "passed." T2 req 2 + literal-path checks exist for this.
- **Stale-exe drift:** the exe snapshots the artefact at compile time — rebuild-then-repackage immediately before handing anything to Joe; Record notes the source commit per exe.
- **In-use files:** Inno retry/abort → abort, close FL, retry; never schedule-on-reboot, never force-close.
- **Unsigned exe friction** on Joe's box — pre-briefed; persistent AV quarantine escalates.
- **Version-parse fragility:** format change ⇒ empty parse ⇒ fail-closed (by design); fix the parse, never hardcode.

## Record
| Task | Commit / session log | Evidence | Reviewer verdict | Joe |
|---|---|---|---|---|
| T1 | IMPLEMENTER 2026-07-07 | see T1 evidence block below | pending | |
| T2 | IMPLEMENTER 2026-07-07 | see T2 evidence block below | pending | |
| T3 | IMPLEMENTER 2026-07-07 | see T3 evidence block below | pending | |
| T4 | (this commit) | FL-closed verified; before-dir captured; install (Joe UAC): 64-bit path literal `Program Files`, 3 keepers + Resources licenses, ARP "GentSampler 1.1.0"; Joe FL smoke PASS incl. clean FL close; models dir untouched (8 files/1.67GB before+after); **finding: first uninstall left the empty bundle skeleton** (unins000.exe self-deletes after the empty-dir sweep) -> .iss amended with in-bundle dirifempty entries, exe re-cut (2:04 PM), install+uninstall re-tested: **bundle dir fully removed**; final reinstall + Joe re-ran the Desktop ACL bat; gates green (build+ctest, pluginval SUCCESS) and non-elevated write access to the deployed binary directly proven (File::Open ReadWrite OK) | — | dev-box cycle PASS 2026-07-07 |
| T5 | `90a3c07` (checklist) | INSTALLER_TEST_CHECKLIST.md delivered (self-contained: exe + FL only); Joe ran it on his separate clean machine with GentSamplerSetup-1.1.0.exe (2:04 PM cut, incl. the dirifempty fix) | — | **PASS (Joe, 2026-07-07: "seems like we are all good" — itemized report-back waived by Joe; install + FL + separation confirmed working on the clean box).** Closes PACKAGING P3's deferred rows per OQ-INST-5 scope (no-toolchain clean machine, Joe FL round-trip, first-run model download). |
| T6 | | | | |

### T1 evidence (payload definition + pre-flight rule)

**Payload table**

| File | Destination in installer | Source |
|---|---|---|
| `GentSampler.vst3` (the plugin binary, inside the bundle) | `{commoncf}\VST3\GentSampler.vst3\Contents\x86_64-win\GentSampler.vst3` | staged artefact dir |
| `onnxruntime.dll` | `...\Contents\x86_64-win\onnxruntime.dll` | staged artefact dir |
| `onnxruntime_providers_shared.dll` | `...\Contents\x86_64-win\onnxruntime_providers_shared.dll` | staged artefact dir |
| `THIRD_PARTY_LICENSES\` (tree: README.md, basic-pitch, demucs, juce, onnxruntime, signalsmith-stretch) | `...\Contents\Resources\THIRD_PARTY_LICENSES\` | repo root |
| `README.md` | `...\Contents\Resources\README.md` | repo root |

No models, no CUDA DLLs, no Standalone component — verified absent below.

**Live `dir` of the staged artefact folder** (`D:\GentSamplerBuild\GentSampler_artefacts\Release\VST3\GentSampler.vst3\Contents\x86_64-win`, captured 2026-07-07 via PowerShell `dir`):

```
    Directory: D:\GentSamplerBuild\GentSampler_artefacts\Release\VST3\GentSampler.vst3\Contents\x86_64-win

Mode                 LastWriteTime         Length Name
----                 -------------         ------ ----
-a----          7/7/2026  12:53 PM        6946816 GentSampler.vst3
-a----         6/25/2026   5:35 PM       11624416 onnxruntime.dll
-a----         6/25/2026   5:35 PM          21472 onnxruntime_providers_shared.dll
```

Exactly the 3 expected keepers, nothing else — matches PACKAGING's proof, no CUDA names present. (`...\Contents\Resources` exists in the bundle today as an empty dir — the correct target for the license tree + README.)

**Pre-flight allowlist (7 CUDA patterns, checked-for-absence) + 3-keeper (checked-for-presence) rule, to be written verbatim into T3's `make_installer.bat`:**
- Keepers required present: `GentSampler.vst3`, `onnxruntime.dll`, `onnxruntime_providers_shared.dll`.
- Patterns required absent (any match = exit non-zero, no compile): `cublas*.dll`, `cudart*.dll`, `cudnn*.dll`, `cufft*.dll`, `curand*.dll`, `nvrtc*.dll`, `onnxruntime_providers_cuda.dll`.

**Models-dir independence note:** the installer never references `Documents\GentSampler` in any section. Verified by source citation — `Source/PluginProcessor.cpp:936` (`gentCheckStemEngine` one-time check) and `Source/PluginProcessor.cpp:1325` (stem-separation job) both resolve the models directory as `juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("GentSampler").getChildFile ("models")` — a per-user special-folder lookup resolved at runtime, wholly independent of where the VST3 bundle itself is installed. Installing to `{commoncf}\VST3\...` has no effect on model resolution.

### T2 evidence (`installer\GentSampler.iss` authoring)

**Inno Setup version installed (winget, this machine):** 6.7.3, at
`C:\Users\JoeyD\AppData\Local\Programs\Inno Setup 6\ISCC.exe` (a per-user
winget install — NOT under `%ProgramFiles(x86)%`; `where ISCC` alone does not
find it because that dir is not on PATH. `make_installer.bat`'s ISCC-locate
step in T3 accounts for this exact path). ISCC banner:
`Inno Setup 6 Command-Line Compiler ... Compiler engine version: Inno Setup 6.7.3`.
Since 6.7.3 ≥ 6.3, `ArchitecturesAllowed=x64compatible` /
`ArchitecturesInstallIn64BitMode=x64compatible` (the modern directive names)
are used, not the legacy `x64` token.

**AppId GUID** (generated once via PowerShell `[guid]::NewGuid()`, committed
in `installer\GentSampler.iss`'s `[Setup]` section): `{34A283D6-272B-491E-84BD-AFD079B239F7}`.

**Compile WITHOUT `/DAppVer`** (expect failure via the `#error` guard):
```
Preprocessing
   Reading file: C:\Users\JoeyD\AppData\Local\Programs\Inno Setup 6\ISPPBuiltins.iss
Error on line 12 in C:\Users\JoeyD\Desktop\GentSampler\GentSampler\installer\GentSampler.iss: "AppVer must be defined on the command line, e.g. ISCC /DAppVer=1.1.0 GentSampler.iss -- do not hardcode a version here."
Compile aborted.
```
Exit code 2 (failure), as required.

**Compile WITH `/DAppVer=1.1.0`** (expect success):
```
Successful compile (3.594 sec). Resulting Setup program filename is:
C:\Users\JoeyD\Desktop\GentSampler\GentSampler\dist\GentSamplerSetup-1.1.0.exe
```
`[Files]` section compiled and packed exactly the T1 payload (3 keepers from
the staged artefact dir + the full `THIRD_PARTY_LICENSES` tree +
`README.md`), confirmed by the compressing-file log lines emitted during this
run (`GentSampler.vst3`, `onnxruntime.dll`, `onnxruntime_providers_shared.dll`,
`THIRD_PARTY_LICENSES\README.md` + all 5 sub-license files, and the repo-root
`README.md`) — no other files. This `dist\GentSamplerSetup-1.1.0.exe` was
produced solely as T2's AC-T2(1) verification artefact; T3 defines the
gitignore rule and is the task that owns the real end-to-end production run.

All seven `.iss` requirements are present in the committed file: `#ifndef
AppVer`/`#error` guard; `AppVersion`/`OutputBaseFilename` from `{#AppVer}`;
`PrivilegesRequired=admin` + the two 64-bit architecture directives; fixed
`DefaultDirName={commoncf}\VST3\GentSampler.vst3` + `DisableDirPage=yes`;
stable committed `AppId`; no `LicenseFile` directive anywhere; no
`[UninstallDelete]`/`[Registry]`/`[Run]` sections and no force-close
directives (Inno's default abort/retry dialog is relied on for in-use files).

### T3 evidence (`installer\make_installer.bat` + `dist/` gitignore)

**Script flow implemented exactly as specified:** parse VERSION from
CMakeLists.txt (`findstr /r` locates the `^project(GentSampler VERSION` line,
then two `for /f` passes strip it down to the bare `x.y.z` token) -> fail
closed if empty -> T1 pre-flight (3 keepers present / 7 CUDA patterns absent)
-> locate ISCC (`where ISCC`, then `%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe`,
then `%LocalAppData%\Programs\Inno Setup 6\ISCC.exe` — see deviation note
below — then fail with install instructions) -> compile with
`/DAppVer=%VER%` -> emit `dist\GentSamplerSetup-%VER%.exe`. Never triggers a
build; a missing/stale artefact dir is a loud non-zero exit.

**Landmine found + fixed during T3 (worth recording for future batch work):**
the first draft's 7-CUDA-pattern check used
`for %%P in (cublas*.dll cudart*.dll ...) do ( for %%F in ("%ARTEFACT_DIR%\%%P") do ... )`.
cmd.exe filesystem-expands wildcard tokens in a `for`'s `in (...)` set
against the *current working directory* before iterating — since none of
those patterns match anything in `installer\`, every wildcard list item
silently vanished from the loop (only the one non-wildcard pattern,
`onnxruntime_providers_cuda.dll`, ever actually got checked), so a planted
`cudnn64_8.dll` in a scratch staging dir went undetected on first test.
Fixed by looping non-wildcard prefixes (`cublas cudart cudnn cufft curand
nvrtc`) and matching each via `dir /b /a-d "%ARTEFACT_DIR%\%%P*.dll"`, which
matches against the explicit staged-dir path with no CWD dependency; the
7th pattern (`onnxruntime_providers_cuda.dll`, no wildcard) is checked
directly with `if exist`. Re-verified against both the real staged dir (no
hit) and a scratch dummy (hit) after the fix — see below.

A second, smaller landmine: `echo [FAIL] ... file(s): ...` inside a
parenthesized `if defined ... ( ... )` block broke cmd's block parser
(`file(s)` is unquoted text with an unbalanced-looking `(`/`)` pair inside an
`if (...)` block — cmd's parser trips over embedded, unquoted parens there
even though they are balanced). Fixed by rewording to "file or files"
(two occurrences: the missing-keeper message and the CUDA-hit message).

**AC-T3(1) — real green run, positive test:**
```
============================================================
GentSampler installer build
============================================================
[OK] Parsed version: 1.1.0
[OK] All 3 keeper files present.
[OK] No CUDA payload files present.
[OK] Using ISCC: "C:\Users\JoeyD\AppData\Local\Programs\Inno Setup 6\ISCC.exe"
...
Successful compile (3.657 sec). Resulting Setup program filename is:
C:\Users\JoeyD\Desktop\GentSampler\GentSampler\dist\GentSamplerSetup-1.1.0.exe
============================================================
[OK] Installer built: "C:\Users\JoeyD\Desktop\GentSampler\GentSampler\installer\..\dist\GentSamplerSetup-1.1.0.exe"
============================================================
```
Confirmed via `dir`: `dist\GentSamplerSetup-1.1.0.exe` exists (6,800,140
bytes), version parsed and filename correct — ran after this same session's
`build_test.sh` gate rebuilt the artefact (payload re-checked at 3 keepers,
no CUDA, both before and after that rebuild).

**AC-T3(2) — four failure modes, each exercised once, all exit non-zero:**

1. **Version-parse-empty** (scratch COPY of CMakeLists.txt, in the session
   scratchpad — `project(GentSampler)` with the ` VERSION x.y.z` clause
   removed, real CMakeLists.txt never touched):
   ```
   [FAIL] Could not find a "project(GentSampler VERSION ...)" line in CMakeLists.txt
   ```
   Exit code 1.
2. **Missing keeper** (scratch STAGING COPY of the artefact tree, built fresh
   under the session scratchpad — real artefact dir never touched — with
   `onnxruntime_providers_shared.dll` absent):
   ```
   [OK] Parsed version: 1.1.0
   [FAIL] Missing required keeper file or files:  onnxruntime_providers_shared.dll
   ```
   Exit code 1.
3. **Planted CUDA dummy** (same scratch staging copy, all 3 keepers restored,
   a zero-byte `cudnn64_8.dll` added):
   ```
   [OK] Parsed version: 1.1.0
   [OK] All 3 keeper files present.
   [FAIL] CUDA payload file or files detected in staged artefact dir -- refusing to package:  cudnn64_8.dll
   ```
   Exit code 1.
4. **ISCC absent** (PATH-shadow, per spec allowance: the real dev box already
   has no Inno Setup dir on PATH, so this test shadowed only the two fallback
   env vars — `LocalAppData` and `ProgramFiles(x86)` — to a directory with no
   ISCC.exe, run against the REAL repo/artefact dir since the failure is
   purely about ISCC discovery, not payload state):
   ```
   [OK] Parsed version: 1.1.0
   [OK] All 3 keeper files present.
   [OK] No CUDA payload files present.
   [FAIL] ISCC.exe not found on PATH or in the standard Inno Setup 6 install locations.
           Install Inno Setup 6 from https://jrsoftware.org/isinfo.php and retry,
           or add ISCC.exe's directory to PATH.
   ```
   Exit code 1.

**AC-T3(3) — `dist/` gitignored, clean status:** `.gitignore` gained a
`dist/` line (alongside `build/`/`out/`/`cmake-build-*/`). `git status
--porcelain` after the full positive run showed only the two intended T3
file-plan changes (`M .gitignore`, `?? installer/make_installer.bat`) —
`dist\GentSamplerSetup-1.1.0.exe` did not appear as untracked.

**AC-T3(4) — repo gates green:** `bash .claude/hooks/build_test.sh` ->
Release build succeeded, ctest `100% tests passed, 0 tests failed out of 1`
(EngineMath, 0.02s). `bash .claude/hooks/pluginval_gate.sh` -> strictness-5
run completed through every stage (cold/warm open, plugin info, programs,
audio processing at 3 sample rates x 5 block sizes, plugin state,
automation, automatable parameters, bus enumeration/enable/disable/restore)
ending `SUCCESS`; no cold-open timeout.

**Deviation from the literal T3 flow text (flagged, not hidden):** the spec's
ISCC-locate step names two lookup steps (`where ISCC`, then
`%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe`). This machine's actual winget
install landed at the per-user `%LocalAppData%\Programs\Inno Setup 6\ISCC.exe`
(confirmed in T2's evidence), which is neither of those two locations. A
third fallback check for that exact path was added so the script actually
locates ISCC on this authoring machine without requiring a manual PATH edit;
the fail-closed behavior (loud non-zero exit with install instructions) is
unchanged if none of the three locations resolve. Flagged for reviewer
attention rather than silently folded in.
