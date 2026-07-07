# PACKAGING_SPEC — FINAL (planner-owned; **Joe-APPROVED 2026-07-07**)

## APPROVAL RECORD (Joe, 2026-07-07)
1. **P2 execution:** agent-executed delete after Joe approves the dry-run listing — APPROVED (spec default stands).
2. **JUCE license mode:** NOT yet declared — Joe: "we need to take care of this kind of thing for sure." P4 therefore delivers the factual entry with the mode bracketed UNRESOLVED **plus a clear decision brief for Joe** (the three JUCE-8 regimes — AGPLv3, free tier w/ splash + revenue cap, paid commercial — with what each implies for a distributed closed-source plugin and the missing top-level LICENSE). The declaration is a follow-up Joe decision, deliberately not blocking this pass.
3. **Version 1.1.0:** APPROVED (CMake → 1.1.0, README H1 stays v1.1).
4. **Clean machine:** EXISTS — Joe has a separate machine. P3 upgrades per the OQ-5 ruling: agents cannot drive that box, so P3 delivers (a) the local scratch-clone proxy run as specced AND (b) a self-contained CLEAN_MACHINE_CHECKLIST (prereq winget installs → clone → build.bat → expected results → pluginval if available → FL smoke) for Joe to execute there; Joe's results complete the ledger's PROVEN column.

**Repo:** `C:\Users\JoeyD\Desktop\GentSampler\GentSampler` @ `0e89d5e`.
**Provenance:** drafted by SPEC-DRAFTER; reviewed, corrected, and OWNED by the PLANNER 2026-07-07. Sources: CLAUDE.md landmines, BACKLOG.md packaging item, WAVE4_SPEC.md F4/F5 records + Risks, build.bat, CMakeLists.txt, README.md, THIRD_PARTY_LICENSES/README.md, GPU_HANDOFF.md, Source/StemSeparator.cpp (`kCudaDlls`), Source/ModelDownloader.cpp.

**Objective** — Make GentSampler installable and functional on a machine that never had the CUDA/cuDNN dev pack: purge the pack from the artefact-bundle source (P1) and the polluted deployed folder (P2), prove the clone → build.bat → pluginval → FL path end-to-end with honest limits (P3), complete the third-party license file (P4), and verify README accuracy + reconcile the version stamp (P5). This pass creates the prerequisites for the follow-on INSTALLER spec; it does not build an installer.

---

## Planner rulings (the six open questions)

- **OQ-PKG-A (installer) — RULING: DEFERRED to a dedicated follow-on spec.** No tooling exists, an installer framework is a NEW dependency requiring Joe's explicit authorization, and the README's shipped install path is build.bat. This pass produces the installer's prerequisites (CUDA-free payload, complete licenses, accurate README, reconciled version); the tee-up is a spec-close deliverable (see "Spec-close"): rewrite BACKLOG.md's packaging item into an INSTALLER item listing payload definition, tooling choice (Joe authorizes), first-run-downloader interplay, and code-signing as its open questions.
- **OQ-PKG-B (system-path delete) — RULING: dry-run-then-approve, agent-executed, is the spec default.** The delete is a small exact-name set, the approval is JOE's approval of the pasted listing (not the lead's), and agent execution keeps the before/after evidence chain in the record and the FL re-check in the same loop. Joe may override to manual-delete at spec approval; if he does, the implementer still produces the dry-run listing and the post-delete verification `dir`.
- **OQ-PKG-C (JUCE license mode) — RULING: facts determined, mode flagged to Joe.** Evidence: CMakeLists.txt fetches JUCE 8.0.4 vanilla (lines 10–14); no commercial license key and no `JUCE_DISPLAY_SPLASH_SCREEN` define exist anywhere in the repo; GentSampler itself has NO top-level LICENSE file. JUCE 8 is dual-licensed (AGPLv3 / commercial, with a free tier). Which regime GentSampler distributes under is a business declaration only Joe can make — P4 writes the factual entry and leaves the mode as a single bracketed line for Joe. Not an implementer guess, ever.
- **OQ-PKG-D (version) — RULING: reconcile NOW, direction CMake → README.** `project(GentSampler VERSION 1.0.0)` is what FL and pluginval display; README has publicly claimed v1.1 since the Phase-2 feature set. The host-visible version should not understate the shipped feature set: P5 changes CMakeLists.txt line 2 to `VERSION 1.1.0` (one line; gates rebuild and pluginval re-reads it). Joe may override the number at approval; the invariant that MUST hold either way: CMake VERSION and README H1 agree at pass close.
- **OQ-5 (P3 proxy ceiling) — RULING: the scratch-clone proxy stands, with an explicit proven-vs-proxied ledger (see P3).** No second machine is confirmed. If Joe offers a machine/VM at approval, P3 upgrades to the true clean-machine protocol (winget setup → clone → build.bat → pluginval → FL if present) and the ledger shrinks accordingly.
- **OQ-6 (Standalone folder) — RULING: AC line, not note.** It costs one `dir` and converts the drafter's session observation into recorded evidence. All of the draft's disk claims are re-verified by the implementer this pass — none are inherited.

**Planner corrections to the draft:** (1) the "13 DLLs" claim lists 12 names — resolved via the authoritative-dir + pattern-allowlist rule below; (2) P1 demoted from BULK-candidate to IMPLEMENTER (irreversible deletes on a non-git path with a known listing discrepancy fail Rule 1's "machine catches it" test); (3) added the purge-permanence statement (nothing re-creates the pack: POST_BUILD copies 2 core DLLs only, deploy copies the binary only, ModelDownloader has no CUDA code); (4) P2's "exactly the 3 keepers" AC reframed subtractively (unknown benign files must not force a violation); (5) added the short-path landmine to P3's scratch clone; (6) version reconciliation ruled in and moved into P5 with a concrete edit.

---

## The delete allowlist (P1 and P2 — single definition)

The live `dir` of each target folder is AUTHORITATIVE; every name list in this spec is a recap. Delete a file only if **both**: it is present on disk, **and** its name matches one of these patterns (identical to build.bat's F5 exclusion set):

```
cublas*.dll  cudart*.dll  cudnn*.dll  cufft*.dll  curand*.dll  nvrtc*.dll
onnxruntime_providers_cuda.dll
```

Expected matches (11–13 files): `cublas64_11.dll, cublasLt64_11.dll, cudart64_110.dll, cudnn64_8.dll, cudnn_ops_infer64_8.dll, cudnn_cnn_infer64_8.dll, cudnn_adv_infer64_8.dll, cufft64_10.dll, curand64_10.dll, nvrtc64_112_0.dll, nvrtc-builtins64_118.dll, onnxruntime_providers_cuda.dll`.

Special cases:
- `zlibwapi.dll` if present: CUDA-pack-only dependency (cuDNN 8.9; the core ORT CPU path does not use it) — include in the dry-run listing FLAGGED, delete with the pack.
- **KEEPERS — never touch:** `GentSampler.vst3` (the binary), `onnxruntime.dll`, `onnxruntime_providers_shared.dll`, and ANY file not matching the patterns (.pdb, manifests, anything unexpected).
- Any on-disk DLL that matches none of {keepers, patterns, zlibwapi.dll} → **STOP and report** before deleting anything.

**Purge permanence (verified this pass by the planner):** POST_BUILD copies only the 2 core ORT DLLs (CMakeLists.txt 116–130); the dev-box deploy step copies only the binary (148–152); ModelDownloader.cpp contains no CUDA download code. Nothing regenerates the pack after either purge.

---

## P1 — Purge the CUDA pack from the artefact-bundle source (IMPLEMENTER)

Target: `D:\GentSamplerBuild\GentSampler_artefacts\Release\VST3\GentSampler.vst3\Contents\x86_64-win\` (build/ junction target — this is the generated-artefact exception to "don't touch build/ by hand", authorized by this spec for the allowlist names only).

**AC-P1:**
1. Before-`dir` pasted into the Record (resolves the 12-vs-13 count on the spot).
2. Delete per the allowlist rule; after-`dir` = before-`dir` minus exactly the pattern-matching files.
3. Fresh `cmake --build build --config Release` resurrects NO purged name (post-rebuild `dir` diff pasted). Note: the rebuild's deploy step will merge-copy the binary to Program Files — normal; if FL holds the plugin the deploy fails = deliberate gate, report and stop.
4. Standalone artefact folder (`...\GentSampler_artefacts\Release\Standalone\`) `dir` pasted, asserting zero pattern-matching names (OQ-6 ruling; drafter observed it clean — re-verify, don't inherit).
5. Gates green after rebuild.

## P2 — Purge the CUDA pack from the DEPLOYED folder (IMPLEMENTER, approval-gated)

Target: `C:\Program Files\Common Files\VST3\GentSampler.vst3\Contents\x86_64-win\` — live system path FL loads from, outside git, irreversible.

**Protocol (non-negotiable):**
1. FL Studio closed. If any file is locked: report and stop. **No agent ever terminates FL Studio or any user application.**
2. Dry-run: paste the full `dir` + the exact to-delete list (allowlist rule above, zlibwapi flagged if present). **STOP. The delete executes only after Joe's explicit approval of that exact listing is on record.**
3. Execute the approved delete — nothing else under Program Files touched, keepers untouched.

**AC-P2:** (1) dry-run listing + Joe's approval on record; (2) post-delete `dir` = pre-delete minus exactly the approved list; (3) Joe: FL loads GentSampler with no missing-DLL error AND one CPU separation completes; (4) permanence note recorded (nothing re-creates the pack — see above).
No retry loop: any failure or surprise escalates to the planner.

## P3 — Clean-machine validation, honest proxy (IMPLEMENTER)

Runs AFTER P2 so the scratch build.bat install exercises F5's exclusion against the real, clean destination.

**Protocol:**
1. Fresh `git clone` of `0e89d5e`+ into a **SHORT path on D:** (e.g. `D:\gspkg`) — deep scratch paths hit MSVC FileTracker `FTK1011` (2026-07-03 landmine); C: has no headroom.
2. Run `build.bat` end-to-end unattended: FetchContent downloads (JUCE 8.0.4, signalsmith main, ORT 1.18.1 zip) from zero local state → Release build → robocopy install. Configure log must show the deploy-step STATUS line (ENABLED on this box — record it).
3. Verify the install destination gained zero pattern-matching DLLs (dir-diff against P2's post-delete listing) and the binary + 2 core DLLs are current.
4. pluginval strictness 5 run directly against the scratch artefact `.vst3`. Any "Open plugin (cold)" timeout = REAL failure (2026-07-04 landmine), never retried; note that the hang class is structurally gone (construct touches no CUDA DLLs — `kEnableCuda=false` gate).
5. Joe FL round-trip: load, drop a track, pads, one CPU separation.
6. OPTIONAL (best-effort): non-elevated build.bat run to exercise the errorlevel-8 → `Documents\VST3` fallback. Caveat: the 2026-07-02 ACL grant may make the non-elevated Program Files copy succeed on this box, making the branch unexercisable — record whichever happens. If the fallback DOES fire, delete `%USERPROFILE%\Documents\VST3\GentSampler.vst3` afterward (user-space; still record before/after) so FL never sees a duplicate plugin.
7. Delete the scratch clone after evidence capture (D: disk hygiene).

**AC-P3:** (1) scratch clone + build.bat completes unattended with a CUDA-free destination; (2) pluginval 5 passes on the scratch artefact; (3) Joe FL round-trip green; (4) a **proven-vs-proxied ledger** in the Record — PROVEN: fresh-clone dependency fetch/configure/build, F5 exclusion at the real destination, pluginval on a from-scratch artefact; PROXIED: the `IS_DIRECTORY` negative branch (stands on WAVE4 F4's 3-case `cmake -P` proof — unexercisable here because the absolute-path destination exists and will NOT be renamed/removed to force it), the no-toolchain winget steps, and (if the ACL grant defeats it) the non-admin fallback. If Joe supplies a machine/VM at approval, this task upgrades to the true clean-machine run and the ledger shrinks.

## P4 — THIRD_PARTY_LICENSES completeness (IMPLEMENTER)

Add entries to `THIRD_PARTY_LICENSES/README.md` matching the Basic Pitch entry's format:
1. **JUCE 8.0.4** — factual half per OQ-PKG-C ruling (vanilla FetchContent, no key, no splash define, dual AGPLv3/commercial upstream); mode = one bracketed line for Joe's declaration. Also note GentSampler has no top-level LICENSE — under an AGPLv3 election that file becomes mandatory (flag, don't create).
2. **Signalsmith Stretch** — MIT, Signalsmith Audio, fetched from `main` (cite repo URL from CMakeLists).
3. **ONNX Runtime 1.18.1** — MIT, Microsoft (cite release URL from CMakeLists).
4. **htdemucs model weights** (`htdemucs_ft` + `htdemucs_6s`) — cite GPU_HANDOFF.md ("MIT models only", HF mirror) + upstream Demucs (Meta).

**AC-P4:** (1) four entries, each citing a verifiable source; (2) JUCE mode either Joe-confirmed or explicitly bracketed UNRESOLVED — never guessed; (3) Basic Pitch entry byte-identical; (4) full license texts included where the format requires them (mirror the basic-pitch/ subfolder pattern for MIT texts).
Escalation expected if the JUCE question grows: that is the planned path, not a failure.

## P5 — README accuracy + version reconciliation (IMPLEMENTER)

1. Diff-read README's Build/Troubleshooting/Features claims against post-F4/F5 reality; correct stale statements or record explicit "verified, no change" per section. Known checkpoints: build.bat behavior (robocopy + fallback), the 10-minute setup, "installs to Common Files\VST3", honest-notes section.
2. **OQ-PKG-D ruling:** edit CMakeLists.txt line 2 → `project(GentSampler VERSION 1.1.0)` (or Joe's override number). README H1 and CMake VERSION must agree at close. Gates rebuild; confirm pluginval/FL report the new version.
3. While in CMakeLists: the comments at lines 27–28 and 133–135 claim ModelDownloader downloads/parks the CUDA pack — stale (no such code). Correct those comment lines ONLY (no behavior change); this prevents the next audit re-flagging a phantom re-creation path.

**AC-P5:** (1) section-by-section verification recorded; (2) `project(... VERSION ...)` and README H1 agree; (3) gates green after the version bump; (4) stale-comment fix touches comments only (diff shows zero non-comment CMake changes).

---

## File plan

| Path | Task |
|---|---|
| `D:\GentSamplerBuild\GentSampler_artefacts\Release\VST3\...\x86_64-win\` (deletes only, allowlist) | P1 |
| `C:\Program Files\Common Files\VST3\GentSampler.vst3\Contents\x86_64-win\` (deletes only, allowlist, Joe-approved) | P2 |
| Scratch clone `D:\gspkg` (created + destroyed) | P3 |
| `THIRD_PARTY_LICENSES/README.md` + new license-text subfolders | P4 |
| `README.md`, `CMakeLists.txt` (line 2 + the two stale comment blocks only) | P5 |
| `PACKAGING_SPEC.md` (Record), `BACKLOG.md` (installer tee-up), `CLAUDE.md` (close) | all |

Nothing else. Any file outside this table = reviewer FAIL.

## UI acceptance criteria
None — no UI change in this pass. The editor lifecycle is covered by pluginval strictness 5 (P3 AC-2) and Joe's FL round-trips (P2 AC-3, P3 AC-3).

## Verification commands
Per checkpoint: `bash .claude/hooks/build_test.sh` then `bash .claude/hooks/pluginval_gate.sh` (defined in `.claude/agent.config`, Rule 10). P3 additionally runs pluginval directly against the scratch artefact. P1/P2's dir-diffs and P3's ledger are evidence the gates cannot produce — the implementer performs them, the reviewer independently re-checks them (WAVE4 precedent).

## Tier assignment (Rule 1)
- **P1 IMPLEMENTER** — demoted from the draft's BULK-candidate: irreversible deletes on a non-git path, with a proven listing discrepancy (12 vs 13) requiring judgment on unexpected names; a wrong delete of a keeper is only partially machine-caught.
- **P2 IMPLEMENTER**, dry-run/approval split as specified. **No BULK anywhere near Program Files.**
- **P3 IMPLEMENTER** (judgment on the ledger). **P4 IMPLEMENTER** (license facts; escalation path live). **P5 IMPLEMENTER.**
- No BULK assignments this pass; no task has a machine check strong enough to justify one.

## Loop budget
Defaults 3/2 from `.claude/agent.config` for P3/P4/P5. Tightened for irreversible work: **P1 = 2 attempts** (second failure = spec problem, escalate), **P2 = 1 attempt** (any failure or surprise after approval escalates immediately — no autonomous retry against a system path). Budget exhaustion = spec-sizing escalation, never more attempts.

## Checkpoints (Rule 9)
Order **P1 → P2 → P3 → P4 → P5** (source clean, deployed clean, then validate against the clean state, then document it). One checkpoint commit per task, `PACKAGING #N: ...`. P1/P2/P3 produce little/no repo diff: their checkpoint commit = this file's Record row + pasted evidence (dir listings, approval, ledger). P4/P5 are normal code/doc commits. Resume: P1/P2 from the Record + live dir state (the dirs themselves are the state); P3–P5 from git. Aborted dispatch → dir-diff/git-diff against the last checkpoint before continuing (2026-07-02 landmine).

## Ground rules (Waves 1–4 protocol, unchanged)
- Deploy-step failure while FL holds the plugin = deliberate gate: report and stop. **No agent ever terminates FL Studio or any user application.**
- pluginval "Open plugin (cold)" timeout = REAL failure; never retry-loop it.
- **Any delete under `C:\Program Files\...` requires a dry-run listing approved by JOE before execution** (new rule, load-bearing for P2).
- `build/` remains hands-off EXCEPT the P1 allowlist deletes this spec explicitly authorizes.

## Out of scope
Actual installer build (deferred, OQ-PKG-A — BACKLOG tee-up only); `kEnableCuda` / ORT pin / any GPU work; JUCE license-mode CHANGE or splash/key configuration (documenting only); creating a GentSampler LICENSE file (flag only); any UI/feature/behavior change beyond the CMake version line; v2 classifier; the CMake deploy-step mechanism itself (F4/F5 landed and stay as-is).

## Spec-level acceptance
1. Artefact source + Standalone folder CUDA-free, rebuild-stable (P1).
2. Deployed folder CUDA-free via Joe-approved delete, FL-confirmed working (P2).
3. Scratch-clone build.bat → CUDA-free install, pluginval 5, Joe FL round-trip, proven-vs-proxied ledger recorded (P3).
4. Licenses complete; JUCE mode Joe-declared or explicitly bracketed (P4).
5. README verified; CMake VERSION == README version (P5).
6. Reviewer verdict.json PASS, 0 blocking; verdict_lint 0.
7. BACKLOG installer item rewritten as the follow-on tee-up; CLAUDE.md "Current state" updated (spec-close).

## Risks
- **P2 is the highest-blast-radius step** (irreversible system-path delete): Joe's approval of the exact listing is load-bearing; the pattern allowlist + subtractive AC + stop-on-surprise rule bound it.
- **The 12-vs-13 discrepancy** means any pre-listed delete set is untrustworthy — hence live-dir authority. If the on-disk set diverges beyond zlibwapi, the stop-and-report rule fires before harm.
- **P3's ledger is honest, not flattering:** the `IS_DIRECTORY` negative branch and the no-toolchain path remain proxied unless Joe supplies a machine. Do not let the record overclaim.
- **P4 JUCE licensing is a legal fact** with a missing GentSampler LICENSE compounding it — flag over guess; expect one escalation.
- **P5 version bump** changes the host-visible plugin version; FL projects saved against 1.0.0 should reload fine (same PLUGIN_CODE), but Joe's round-trip after P5 should include reopening an existing project.

## Record
| Task | Commit / session log | Evidence | Reviewer verdict | Joe |
|---|---|---|---|---|
| P1 | — | — | — | — |
| P2 | — | — | — | — |
| P3 | — | — | — | — |
| P4 | — | — | — | — |
| P5 | — | — | — | — |
